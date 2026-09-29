#include "rxe/render/core/renderer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <base/check.h>
#include <base/option.h>

#include "base/containers/span.h"
#include "base/containers/vector.h"
#include "base/functional/function.h"
#include "base/memory/mem_ops.h"
#include "base/memory/move.h"
#include "base/memory/unique_pointer.h"
#include "base/optional.h"
#include "base/strings/xstring.h"
#include "base/time/time.h"
#include "foundation/algorithm/sort.h"
#include "foundation/logging/log.h"
#include "foundation/math/scalar.h"
#include "foundation/memory/memory_tracker.h"
#include "rxe/asset/primitives.h"
#include "rxe/asset/texture_compress.h"
#include "rxe/asset/exr_write.h"
#include "rxe/render/core/renderer_internal.h"

namespace rx::render {
namespace {

bool SupportsStaticInstances(const gpu::GpuMesh &mesh,
                             const MaterialSystem *materials) {
  if (mesh.all_blend || mesh.skinned || mesh.morph_target_count > 0 ||
      mesh.terrain_lod || mesh.dynamic_vertices)
    return false;
  auto supported = [materials](const base::Vector<gpu::GpuSubmesh> &submeshes) {
    for (const gpu::GpuSubmesh &submesh : submeshes) {
      if (submesh.blend ||
          (materials && materials->is_normal_model_space(submesh.material))) {
        return false;
      }
    }
    return true;
  };
  if (!supported(mesh.submeshes))
    return false;
  for (const gpu::GpuLod &lod : mesh.lods) {
    if (!supported(lod.submeshes))
      return false;
  }
  return true;
}

bool HasUniformScale(base::Span<const Mat4> transforms) {
  for (const Mat4 &transform : transforms) {
    const f32 *m = transform.m;
    for (u32 i = 0; i < 16; ++i)
      if (!isfinite(m[i]))
        return false;
    if (::fabsf(m[3]) > 1e-5f || ::fabsf(m[7]) > 1e-5f ||
        ::fabsf(m[11]) > 1e-5f || ::fabsf(m[15] - 1.0f) > 1e-5f)
      return false;
    const f32 sx = ::sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    const f32 sy = ::sqrtf(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
    const f32 sz = ::sqrtf(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
    const f32 tolerance = rx::Max({sx, sy, sz}) * 1e-4f;
    if (sx <= 1e-6f || ::fabsf(sx - sy) > tolerance ||
        ::fabsf(sx - sz) > tolerance)
      return false;
    const f32 orthogonal_tolerance = sx * sx * 1e-4f;
    const f32 determinant = m[0] * (m[5] * m[10] - m[6] * m[9]) -
                            m[4] * (m[1] * m[10] - m[2] * m[9]) +
                            m[8] * (m[1] * m[6] - m[2] * m[5]);
    if (determinant <= 0 ||
        ::fabsf(m[0] * m[4] + m[1] * m[5] + m[2] * m[6]) >
            orthogonal_tolerance ||
        ::fabsf(m[0] * m[8] + m[1] * m[9] + m[2] * m[10]) >
            orthogonal_tolerance ||
        ::fabsf(m[4] * m[8] + m[5] * m[9] + m[6] * m[10]) >
            orthogonal_tolerance)
      return false;
  }
  return true;
}

// Average opacity of an alpha-masked submesh for the vegetation opaque
// approximation. Samples the material's baked alpha grid at each triangle's
// three vertices, edge midpoints and centroid (7 points in barycentric UV
// space, a cheap stand-in for integrating covered texels over the footprint)
// and area-weights across the submesh. Returns 1.0 (no shrink, i.e. today's
// force-opaque behavior) when the alpha was not decoded: opaque texture, a
// format without a CPU alpha decoder (BC7), or a missing material.
f32 MaskedSubmeshOpacity(const base::Vector<asset::Vertex> &verts,
                         const base::Vector<u32> &indices, const gpu::GpuSubmesh &sm,
                         const MaterialSystem::AlphaCoverage *cov) {
  if (!cov)
    return 1.0f;
  auto S = [&](const f32 uv0[2], const f32 uv1[2], const f32 uv2[2], f32 w0,
               f32 w1, f32 w2) {
    return cov->Sample(uv0[0] * w0 + uv1[0] * w1 + uv2[0] * w2,
                       uv0[1] * w0 + uv1[1] * w1 + uv2[1] * w2);
  };
  f64 weighted = 0.0, area_sum = 0.0;
  for (u32 e = 0; e + 3 <= sm.index_count; e += 3) {
    u32 i0 = indices[sm.index_offset + e],
        i1 = indices[sm.index_offset + e + 1],
        i2 = indices[sm.index_offset + e + 2];
    if (i0 >= verts.size() || i1 >= verts.size() || i2 >= verts.size())
      continue;
    const asset::Vertex &a = verts[i0];
    const asset::Vertex &b = verts[i1];
    const asset::Vertex &c = verts[i2];
    f32 mean_a =
        (S(a.uv, b.uv, c.uv, 1, 0, 0) + S(a.uv, b.uv, c.uv, 0, 1, 0) +
         S(a.uv, b.uv, c.uv, 0, 0, 1) + S(a.uv, b.uv, c.uv, 0.5f, 0.5f, 0) +
         S(a.uv, b.uv, c.uv, 0, 0.5f, 0.5f) +
         S(a.uv, b.uv, c.uv, 0.5f, 0, 0.5f) +
         S(a.uv, b.uv, c.uv, 1.f / 3, 1.f / 3, 1.f / 3)) /
        7.0f;
    f32 e1[3] = {b.position[0] - a.position[0], b.position[1] - a.position[1],
                 b.position[2] - a.position[2]};
    f32 e2[3] = {c.position[0] - a.position[0], c.position[1] - a.position[1],
                 c.position[2] - a.position[2]};
    f32 cx = e1[1] * e2[2] - e1[2] * e2[1], cy = e1[2] * e2[0] - e1[0] * e2[2],
        cz = e1[0] * e2[1] - e1[1] * e2[0];
    f32 area = 0.5f * ::sqrtf(cx * cx + cy * cy + cz * cz);
    weighted += static_cast<f64>(mean_a) * area;
    area_sum += area;
  }
  if (area_sum <= 0.0)
    return cov->mean;
  return static_cast<f32>(weighted / area_sum);
}

} // namespace

void Renderer::UploadMeshletMesh(const asset::Mesh &mesh) {
  if (!device_ || device_->is_stub())
    return;
  meshlet_.Upload(*device_, mesh);
}

void Renderer::UploadVirtualGeometryMesh(const asset::Mesh &mesh) {
  if (!device_ || device_->is_stub())
    return;
  vgeo_.Upload(*device_, mesh);
}

void Renderer::SetVirtualGeometryInstances(base::Span<const Mat4> transforms) {
  vgeo_.SetInstances(transforms);
}

void Renderer::SetInteriorVolumes(base::Span<const InteriorVolume> volumes) {
  interior_volumes_.assign(volumes.begin(), volumes.end());
}

void Renderer::SetVirtualGeometryAlbedo(ByteSpan rgba_mips, u32 size,
                                        f32 world_to_uv) {
  if (!device_ || device_->is_stub())
    return;
  vgeo_.SetAlbedo(*device_, rgba_mips, size, world_to_uv);
}

void Renderer::SeedHairStrands(const Vec3 &head_center, f32 head_radius,
                               u32 strands, f32 length) {
  if (!device_ || device_->is_stub())
    return;
  hair_.SeedCap(*device_, head_center, head_radius, strands, length);
}

u32 Renderer::CreateHairGroom(const asset::Mesh &hair_mesh,
                              const GroomParams &params,
                              const Mat4 &transform) {
  if (!device_ || device_->is_stub())
    return 0;
  GroomData data;
  if (!BuildHairGroom(hair_mesh, params, &data)) {
    RX_WARN("hair groom build failed for mesh");
    return 0;
  }
  return hair_.CreateGroom(*device_, data, params, transform);
}

u32 Renderer::CreateHairGroom(const GroomData &data, const GroomParams &params,
                              const Mat4 &transform) {
  if (!device_ || device_->is_stub())
    return 0;
  return hair_.CreateGroom(*device_, data, params, transform);
}

void Renderer::SetHairGroomTransform(u32 id, const Mat4 &transform) {
  hair_.SetGroomTransform(id, transform);
}

void Renderer::SetHairGroomPoints(u32 id, const f32 *positions, u32 count) {
  hair_.SetGroomPoints(id, positions, count);
}

void Renderer::SetHairGroomTint(u32 id, const Vec3 &tint) {
  hair_.SetGroomTint(id, tint);
}

void Renderer::SetHairGroomMaterial(u32 id, const HairSurfaceParameters &params) {
  hair_.SetGroomHair(id, params);
}

void Renderer::SetHairGroomTier(u32 id, HairTier tier) { hair_.SetGroomTier(id, tier); }

void Renderer::DestroyHairGroom(u32 id) {
  if (!device_ || device_->is_stub())
    return;
  hair_.DestroyGroom(*device_, id);
}

bool Renderer::HairGroomHead(u32 id, Vec3 *center, f32 *radius) {
  return hair_.GroomHead(id, center, radius);
}

u32 Renderer::BakeImposter(const asset::Mesh &mesh) {
  if (!device_ || device_->is_stub())
    return ImposterPass::kNoMesh;
  // The bake samples the base-colour maps the mesh's own materials bind, so
  // they have to be uploaded already - which they are, since a mesh reaches
  // the gpu after its materials.
  base::Vector<ImposterPass::BakeMaterial> materials;
  if (!mesh.lods.empty() && material_system_) {
    for (const asset::Submesh &submesh : mesh.lods[0].submeshes) {
      MaterialSystem::BaseColor base =
          material_system_->material_base_color(submesh.material.hash);
      materials.push_back({base.image, base.alpha_cutoff});
    }
  }
  return imposters_.Bake(*device_, mesh, base::Span(materials.data(), materials.size()));
}

void Renderer::SetImposterInstances(
    base::Span<const ImposterPass::Instance> instances) {
  if (!device_ || device_->is_stub())
    return;
  imposters_.SetInstances(*device_, instances);
}

InstanceGroupHandle
Renderer::CreateInstanceGroup(u64 mesh, base::Span<const Mat4> transforms) {
  if (!device_ || device_->is_stub())
    return {};
  const gpu::GpuMesh *gpu = meshes_.find(mesh);
  if (!gpu || !SupportsStaticInstances(*gpu, material_system_.Get_UseOnlyIfYouKnowWhatYouareDoing()) ||
      mesh_emitters_.find(mesh) || !HasUniformScale(transforms))
    return {};
  InstanceGroupHandle handle = instances_.Create(
      *device_, mesh, transforms, gpu->bounds_center, gpu->bounds_radius);
  if (handle)
    ++scene_revision_;
  return handle;
}

bool Renderer::UpdateInstanceGroup(InstanceGroupHandle handle,
                                   base::Span<const Mat4> transforms) {
  if (!device_ || device_->is_stub() ||
      handle.index >= instances_.groups().size())
    return false;
  const InstanceStore::Group &group = instances_.groups()[handle.index];
  const gpu::GpuMesh *gpu = meshes_.find(group.mesh);
  const bool replaced =
      gpu && HasUniformScale(transforms) &&
      instances_.Replace(*device_, handle, transforms, gpu->bounds_center,
                         gpu->bounds_radius);
  if (replaced)
    ++scene_revision_;
  return replaced;
}

void Renderer::DestroyInstanceGroup(InstanceGroupHandle handle) {
  if (!device_ || device_->is_stub())
    return;
  if (instances_.Destroy(*device_, handle))
    ++scene_revision_;
}

bool Renderer::UploadMesh(const asset::Mesh &mesh, u64 id_salt) {
  if (!device_ || device_->is_stub())
    return false;
  const u64 mesh_key = mesh.id.hash ^ id_salt;
  // Resolve each emitter's texture asset hash to a bindless index (or invalid)
  // so the particle billboards can sample the authored effect texture.
  auto register_emitters =
      [&](const base::Vector<asset::ParticleEmitter> &src) {
        base::Vector<asset::ParticleEmitter> emitters = src;
        for (asset::ParticleEmitter &e : emitters) {
          u32 index = BindlessRegistry::kInvalidIndex;
          if (e.texture != 0 && material_system_) {
            // The index is baked here for the mesh's lifetime; streaming would
            // move the slot under it.
            material_system_->Pin(e.texture ^ id_salt);
            index = material_system_->bindless_texture(e.texture ^ id_salt);
          }
          e.texture = index; // now a bindless index, 0xffffffff = untextured
        }
        mesh_emitters_[mesh_key] = base::move(emitters);
      };
  // Emitter-only NIFs (smoke columns, dust wisps) carry no geometry but still
  // need their particle pools: register the emitters and accept the upload so
  // the placed reference spawns and the sim runs, without a GPU mesh.
  if (mesh.lods.empty() || mesh.lods[0].vertices.empty()) {
    if (mesh.emitters.empty())
      return false;
    register_emitters(mesh.emitters);
    return true;
  }

  // kBufferUsageStorage: the bindless registry mirrors RT mesh buffers into
  // its geometry buffer array (raw reads for the DXIL hit shaders), so the
  // buffers must be descriptor-bindable, not just address-reachable.
  gpu::BufferUsageFlags rt_usage =
      raytracing_ ? (gpu::kBufferUsageAccelBuildInput | gpu::kBufferUsageDeviceAddress |
                     gpu::kBufferUsageStorage)
                  : 0;
  // The mesh-shader path reads vertices/meshlets by device address. Skinned
  // and morphed meshes stay on the raster vertex path, which deforms them.
  const bool build_meshlets = device_->caps().mesh_shaders &&
                              !mesh.dynamic_vertices && !mesh.skinned &&
                              mesh.morph_targets.empty();
  gpu::BufferUsageFlags ms_usage = build_meshlets ? gpu::kBufferUsageDeviceAddress : 0;

  // On the mesh-shader path, synthesize coarse lods for eligible
  // single-material statics so the task stage can drop detail with distance
  // (GenerateLods is a no-op for skinned / multi-submesh / already-multi-lod /
  // tiny meshes). The copy only happens for meshes that will actually get lods.
  asset::Mesh lodded;
  const asset::Mesh *src = &mesh;
  if (!mesh.dynamic_vertices && build_meshlets && mesh.lods.size() == 1 &&
      mesh.lods[0].submeshes.size() <= 1 &&
      mesh.lods[0].indices.size() >= 3000) {
    lodded = mesh;
    asset::GenerateLods(&lodded);
    if (lodded.lods.size() > 1)
      src = &lodded;
  }

  const asset::MeshLod &lod = src->lods[0];
  gpu::GpuMesh gpu;

  // Concatenate every lod into shared vertex/index buffers; each lod keeps its
  // local indices, rebased onto its vertices through the draw's vertexOffset.
  base::Vector<asset::Vertex> all_verts;
  base::Vector<u32> all_indices;
  base::Vector<u32> vertex_bases, index_bases;
  for (const asset::MeshLod &l : src->lods) {
    vertex_bases.push_back(static_cast<u32>(all_verts.size()));
    index_bases.push_back(static_cast<u32>(all_indices.size()));
    for (const asset::Vertex &v : l.vertices)
      all_verts.push_back(v);
    for (u32 idx : l.indices)
      all_indices.push_back(idx);
  }
  gpu.vertices = device_->CreateBufferWithData(
      ByteSpan(reinterpret_cast<const u8 *>(all_verts.data()),
               all_verts.size() * sizeof(asset::Vertex)),
      gpu::kBufferUsageVertex | rt_usage | ms_usage);
  gpu.indices = device_->CreateBufferWithData(
      ByteSpan(reinterpret_cast<const u8 *>(all_indices.data()),
               all_indices.size() * sizeof(u32)),
      gpu::kBufferUsageIndex | rt_usage);
  gpu.index_count =
      static_cast<u32>(lod.indices.size()); // lod 0 (rt/shadow/overdraw)
  gpu.vertex_count = static_cast<u32>(lod.vertices.size());
  // Skinned meshes carry a parallel bone index/weight stream, bound as a second
  // vertex buffer by the skinned pipeline. Skinned meshes are not lod'd.
  // kBufferUsageStorage: the skinned-RT compute pass reads this stream as a
  // ByteAddressBuffer to deform the mesh for ray tracing, not only the raster
  // vertex stage.
  if (mesh.skinned && lod.skinning.size() == lod.vertices.size()) {
    gpu.skinning = device_->CreateBufferWithData(
        ByteSpan(reinterpret_cast<const u8 *>(lod.skinning.data()),
                 lod.skinning.size() * sizeof(asset::SkinnedVertexExtra)),
        gpu::kBufferUsageVertex | gpu::kBufferUsageStorage);
    gpu.skinned = static_cast<bool>(gpu.skinning);
  }
  // Morph target deltas, packed [target][vertex] as {position, normal,
  // tangent} float3 triples and read by device address in the vertex shaders.
  // Targets are authored against lod 0; morphed meshes stay on it (see the
  // cull build below).
  if (!mesh.morph_targets.empty()) {
    const size_t verts = lod.vertices.size();
    base::Vector<f32> deltas(mesh.morph_targets.size() * verts * 9);
    for (size_t t = 0; t < mesh.morph_targets.size(); ++t) {
      const asset::MorphTarget &target = mesh.morph_targets[t];
      f32 *out = deltas.data() + t * verts * 9;
      for (size_t v = 0; v < verts; ++v, out += 9) {
        if (target.position_deltas.size() >= (v + 1) * 3) {
          base::MemCopy(out, &target.position_deltas[v * 3], sizeof(f32) * 3);
        }
        if (target.normal_deltas.size() >= (v + 1) * 3) {
          base::MemCopy(out + 3, &target.normal_deltas[v * 3], sizeof(f32) * 3);
        }
        if (target.tangent_deltas.size() >= (v + 1) * 3) {
          base::MemCopy(out + 6, &target.tangent_deltas[v * 3], sizeof(f32) * 3);
        }
      }
    }
    gpu.morph_deltas = device_->CreateBufferWithData(
        ByteSpan(reinterpret_cast<const u8 *>(deltas.data()),
                 deltas.size() * sizeof(f32)),
        gpu::kBufferUsageStorage | gpu::kBufferUsageDeviceAddress);
    gpu.morph_target_count = static_cast<u32>(mesh.morph_targets.size());
  }
  auto build_submeshes = [&](const asset::MeshLod &l, u32 index_base,
                             base::Vector<gpu::GpuSubmesh> &out) {
    if (l.submeshes.empty()) {
      out.push_back(
          {index_base, static_cast<u32>(l.indices.size()), 0, false, false});
      return;
    }
    for (const asset::Submesh &submesh : l.submeshes) {
      // Store the salted material hash so every later draw-loop lookup matches
      // this domain's materials (uploaded under the same salt).
      u64 material = submesh.material.hash ^ id_salt;
      bool water = material_system_ && material_system_->is_water(material);
      bool blend =
          water || (material_system_ && material_system_->is_blend(material));
      bool mask = material_system_ && material_system_->is_mask(material);
      bool effect = material_system_ && material_system_->is_effect(material);
      bool effect_additive =
          effect && material_system_->is_effect_additive(material);
      gpu::GpuSubmesh out_submesh{index_base + submesh.index_offset,
                             submesh.index_count,
                             material,
                             blend,
                             water,
                             mask};
      out_submesh.effect = effect;
      out_submesh.effect_additive = effect_additive;
      out.push_back(out_submesh);
    }
  };
  build_submeshes(src->lods[0], index_bases[0], gpu.submeshes);
  for (size_t i = 1; i < src->lods.size(); ++i) {
    gpu::GpuLod glod;
    glod.vertex_offset = vertex_bases[i];
    build_submeshes(src->lods[i], index_bases[i], glod.submeshes);
    gpu.lods.push_back(base::move(glod));
  }
  gpu.all_blend = true;
  bool all_water = !gpu.submeshes.empty();
  for (const gpu::GpuSubmesh &submesh : gpu.submeshes) {
    if (!submesh.blend)
      gpu.all_blend = false;
    if (!submesh.water)
      all_water = false;
  }
  if (all_water && lod.vertices.size() >= 4) {
    f32 min_x = lod.vertices[0].position[0], max_x = min_x;
    f32 min_y = lod.vertices[0].position[1], max_y = min_y;
    f32 min_z = lod.vertices[0].position[2], max_z = min_z;
    for (const asset::Vertex &vertex : lod.vertices) {
      min_x = rx::Min(min_x, vertex.position[0]);
      max_x = rx::Max(max_x, vertex.position[0]);
      min_y = rx::Min(min_y, vertex.position[1]);
      max_y = rx::Max(max_y, vertex.position[1]);
      min_z = rx::Min(min_z, vertex.position[2]);
      max_z = rx::Max(max_z, vertex.position[2]);
    }
    const f32 horizontal_extent = rx::Max(max_x - min_x, max_z - min_z);
    const f32 planar_tolerance = rx::Max(0.02f, horizontal_extent * 1e-4f);
    gpu.planar_water =
        horizontal_extent > 1.0f && max_y - min_y <= planar_tolerance;
    if (gpu.planar_water) {
      gpu.water_bounds[0] = min_x;
      gpu.water_bounds[1] = min_z;
      gpu.water_bounds[2] = max_x;
      gpu.water_bounds[3] = max_z;
      gpu.water_height = (min_y + max_y) * 0.5f;
    }
  }
  base::MemCopy(gpu.bounds_center, mesh.bounds_center, sizeof(f32) * 3);
  gpu.bounds_radius = mesh.bounds_radius;
  gpu.no_rt = mesh.exclude_from_rt;
  gpu.terrain_lod = mesh.terrain_lod;
  gpu.dynamic_vertices = mesh.dynamic_vertices;

  // Mesh-shader path: split every opaque submesh of every lod into meshlets,
  // concatenated into shared buffers. Each (lod, submesh) records its meshlet
  // range; the task stage picks a lod by distance and dispatches that range.
  // Meshlet vertex indices are rebased to absolute indices into the shared
  // (lod-concatenated) vertex buffer so the mesh shader pulls the right lod.
  if (build_meshlets) {
    base::Vector<Meshlet> all_meshlets;
    base::Vector<u32> all_mv;
    base::Vector<u32> all_mt;
    auto build_lod = [&](base::Vector<gpu::GpuSubmesh> &subs, u32 vertex_base,
                         u32 vertex_count) {
      for (gpu::GpuSubmesh &submesh : subs) {
        if (submesh.blend || submesh.index_count == 0)
          continue;
        MeshletGeometry geo = BuildMeshletGeometry(
            all_verts.data() + vertex_base, vertex_count,
            all_indices.data() + submesh.index_offset, submesh.index_count);
        if (geo.meshlets.empty())
          continue;
        u32 vtx_base = static_cast<u32>(all_mv.size());
        u32 tri_base = static_cast<u32>(all_mt.size());
        submesh.meshlet_offset = static_cast<u32>(all_meshlets.size());
        submesh.meshlet_count = static_cast<u32>(geo.meshlets.size());
        for (Meshlet m : geo.meshlets) {
          m.vertex_offset += vtx_base;
          m.triangle_offset += tri_base;
          all_meshlets.push_back(m);
        }
        for (u32 v : geo.vertex_indices)
          all_mv.push_back(v + vertex_base); // -> global index
        for (u32 t : geo.triangles)
          all_mt.push_back(t);
      }
    };
    build_lod(gpu.submeshes, vertex_bases[0],
              static_cast<u32>(src->lods[0].vertices.size()));
    for (size_t i = 1; i < src->lods.size(); ++i) {
      build_lod(gpu.lods[i - 1].submeshes, vertex_bases[i],
                static_cast<u32>(src->lods[i].vertices.size()));
    }
    if (!all_meshlets.empty()) {
      gpu.meshlets = device_->CreateBufferWithData(
          ByteSpan(reinterpret_cast<const u8 *>(all_meshlets.data()),
                   all_meshlets.size() * sizeof(Meshlet)),
          ms_usage);
      gpu.meshlet_vertices = device_->CreateBufferWithData(
          ByteSpan(reinterpret_cast<const u8 *>(all_mv.data()),
                   all_mv.size() * sizeof(u32)),
          ms_usage);
      gpu.meshlet_triangles = device_->CreateBufferWithData(
          ByteSpan(reinterpret_cast<const u8 *>(all_mt.data()),
                   all_mt.size() * sizeof(u32)),
          ms_usage);
      gpu.has_meshlets = true;
    }
  }
  // Grass-like fill geometry stays out of the realtime tlas (bloat + noise),
  // but the path tracer wants it (alpha-tested foliage), so include it when
  // path tracing is enabled. Dynamic geometry rebuilds its BLAS and bindless
  // vertex address whenever its same-topology vertex buffer is replaced.
  bool include_rt = !gpu.no_rt || settings_.path_trace;
  // raytracing_ gate: without it the vertex/index buffers were created
  // without device-address usage, so the mesh records would hold null
  // addresses (the registry itself now always exists for textures/materials).
  if (bindless_ && raytracing_ && !gpu.all_blend && include_rt) {
    base::Vector<BindlessRegistry::GeometryRecord> geometries;
    for (const gpu::GpuSubmesh &submesh : gpu.submeshes) {
      if (submesh.blend || submesh.index_count == 0)
        continue;
      geometries.push_back(
          {submesh.index_offset,
           material_system_->bindless_material(submesh.material)});
    }
    const u32 bindless_index =
        bindless_->RegisterMesh(gpu.vertices, gpu.indices, geometries.data(),
                                static_cast<u32>(geometries.size()));
    if (bindless_index != BindlessRegistry::kInvalidIndex) {
      gpu.bindless_index = bindless_index;
      gpu.bindless_geometry = true;
    } else {
      rt_geometry_dirty_ = true;
    }
  }
  // Distance-LOD ray-tracing geometry (RX_RT_LOD_NEAR). For each extra LOD,
  // build an index buffer whose values are rebased to absolute indices into the
  // shared (all-lods-concatenated) vertex buffer, plus a bindless record, so a
  // TLAS instance can point at a coarser LOD past the near radius. Every
  // non-blend submesh is force-OPAQUE here: past the LOD switch the opaque-
  // approximation shrink is imperceptible, so distant masked foliage stays a
  // solid stand-in (no separate approx variant, matching the RAY_FLAG_CULL_NON_
  // OPAQUE realtime paths which would otherwise skip non-opaque geometry). The
  // BLAS is deferred to EnsureLodRtGeometry (first selection); only the cheap
  // index buffer + record are made here, where the CPU geometry is in hand.
  if (bindless_ && raytracing_ && !gpu.all_blend && include_rt &&
      !gpu.lods.empty()) {
    const u32 total_verts = static_cast<u32>(all_verts.size());
    gpu.lod_rt.resize(gpu.lods.size());
    for (size_t li = 0; li < gpu.lods.size(); ++li) {
      const u32 vertex_base = vertex_bases[li + 1]; // vertex_bases[0] = lod0
      base::Vector<u32> lod_indices;
      base::Vector<BindlessRegistry::GeometryRecord> lod_geoms;
      gpu::GpuMesh::LodRt &rt = gpu.lod_rt[li];
      for (const gpu::GpuSubmesh &submesh : gpu.lods[li].submeshes) {
        if (submesh.blend || submesh.index_count == 0)
          continue;
        const u32 offset = static_cast<u32>(lod_indices.size());
        for (u32 e = 0; e < submesh.index_count; ++e)
          lod_indices.push_back(all_indices[submesh.index_offset + e] +
                                vertex_base);
        rt.geoms.push_back({offset, submesh.index_count});
        lod_geoms.push_back(
            {offset, material_system_->bindless_material(submesh.material)});
      }
      if (lod_indices.empty())
        continue;
      rt.vertex_count = total_verts;
      rt.indices = device_->CreateBufferWithData(
          ByteSpan(reinterpret_cast<const u8 *>(lod_indices.data()),
                   lod_indices.size() * sizeof(u32)),
          gpu::kBufferUsageIndex | gpu::kBufferUsageAccelBuildInput |
              gpu::kBufferUsageDeviceAddress | gpu::kBufferUsageStorage);
      if (!rt.indices) {
        rt.geoms.clear();
        continue;
      }
      rt.bindless =
          bindless_->RegisterMesh(gpu.vertices, rt.indices, lod_geoms.data(),
                                  static_cast<u32>(lod_geoms.size()));
    }
  }
  // Opaque-approximation variant for alpha-masked (vegetation) submeshes. Each
  // masked triangle is duplicated with its own three vertices, shrunk about its
  // centroid by sqrt(average opacity) so the stand-in's area matches the
  // average covered fraction, and flagged OPAQUE. Realtime diffuse GI / AO /
  // shadow rays hit this via kRayMaskApprox and skip the real (non-opaque)
  // masked geometry, which the path tracer and reflections keep. RX_RT_VEG=0
  // forces the shrink factor to 1 (identical to the real triangles),
  // reproducing today's force-opaque behavior for A/B. Realtime-tlas meshes
  // only (no_rt fill is path-trace-only and never hits realtime rays).
  base::Vector<gpu::AccelTriangles> approx_accel;
  if (bindless_ && raytracing_ && material_system_ && !gpu.all_blend &&
      !gpu.no_rt) {
    const bool veg = internal::RtVegOpt;
    base::Vector<asset::Vertex> approx_verts;
    base::Vector<u32> approx_indices;
    base::Vector<BindlessRegistry::GeometryRecord> approx_geoms;
    struct ApproxRange {
      u32 index_offset;
      u32 index_count;
    };
    base::Vector<ApproxRange> approx_ranges;
    for (const gpu::GpuSubmesh &submesh : gpu.submeshes) {
      if (!submesh.alpha_mask || submesh.blend || submesh.index_count == 0)
        continue;
      f32 opacity =
          veg ? MaskedSubmeshOpacity(
                    all_verts, all_indices, submesh,
                    material_system_->material_base_alpha(submesh.material))
              : 1.0f;
      const f32 s = ::sqrtf(rx::Clamp(opacity, 0.02f, 1.0f));
      const u32 base_index = static_cast<u32>(approx_indices.size());
      for (u32 e = 0; e + 3 <= submesh.index_count; e += 3) {
        const u32 idx[3] = {all_indices[submesh.index_offset + e],
                            all_indices[submesh.index_offset + e + 1],
                            all_indices[submesh.index_offset + e + 2]};
        if (idx[0] >= all_verts.size() || idx[1] >= all_verts.size() ||
            idx[2] >= all_verts.size())
          continue;
        f32 cen[3] = {0, 0, 0};
        for (u32 k = 0; k < 3; ++k)
          for (u32 c = 0; c < 3; ++c)
            cen[c] += all_verts[idx[k]].position[c] / 3.0f;
        for (u32 k = 0; k < 3; ++k) {
          asset::Vertex v =
              all_verts[idx[k]]; // keep normal/tangent/uv/color for hit shading
          for (u32 c = 0; c < 3; ++c)
            v.position[c] = cen[c] + s * (v.position[c] - cen[c]);
          approx_indices.push_back(static_cast<u32>(approx_verts.size()));
          approx_verts.push_back(v);
        }
      }
      const u32 count = static_cast<u32>(approx_indices.size()) - base_index;
      if (count == 0)
        continue;
      approx_geoms.push_back(
          {base_index, material_system_->bindless_material(submesh.material)});
      approx_ranges.push_back({base_index, count});
    }
    if (!approx_verts.empty()) {
      gpu::BufferUsageFlags approx_usage = gpu::kBufferUsageAccelBuildInput |
                                      gpu::kBufferUsageDeviceAddress |
                                      gpu::kBufferUsageStorage;
      gpu.rt_approx_vertices = device_->CreateBufferWithData(
          ByteSpan(reinterpret_cast<const u8 *>(approx_verts.data()),
                   approx_verts.size() * sizeof(asset::Vertex)),
          gpu::kBufferUsageVertex | approx_usage);
      gpu.rt_approx_indices = device_->CreateBufferWithData(
          ByteSpan(reinterpret_cast<const u8 *>(approx_indices.data()),
                   approx_indices.size() * sizeof(u32)),
          gpu::kBufferUsageIndex | approx_usage);
      if (gpu.rt_approx_vertices && gpu.rt_approx_indices) {
        gpu.rt_approx_bindless = bindless_->RegisterMesh(
            gpu.rt_approx_vertices, gpu.rt_approx_indices, approx_geoms.data(),
            static_cast<u32>(approx_geoms.size()));
        gpu.rt_approx_bindless_valid =
            gpu.rt_approx_bindless != BindlessRegistry::kInvalidIndex;
        if (!gpu.rt_approx_bindless_valid)
          gpu.rt_approx_bindless = 0;
        for (const ApproxRange &r : approx_ranges) {
          approx_accel.push_back(
              {.vertex_address = gpu.rt_approx_vertices.address,
               .vertex_stride = sizeof(asset::Vertex),
               .vertex_count = static_cast<u32>(approx_verts.size()),
               .vertex_format = gpu::Format::kRGB32Float,
               .index_address =
                   gpu.rt_approx_indices.address + r.index_offset * sizeof(u32),
               .index_count = r.index_count,
               .index_type = gpu::IndexType::kUint32,
               .opaque = true});
        }
        gpu.rt_approx = !approx_accel.empty();
      }
    }
  }
  // Re-uploading under an existing key (the builtin biped goes up once for
  // the test spawn and again for the npc template) must free the previous
  // buffers or they leak until vkDestroyDevice complains.
  const bool replacing_mesh = meshes_.find(mesh_key) != nullptr;
  if (gpu::GpuMesh *previous = meshes_.find(mesh_key)) {
    device_->WaitIdle(); // uploads happen at load time; never per frame
    skinned_rt_.InvalidateMesh(
        *device_, raytracing_.Get_UseOnlyIfYouKnowWhatYouareDoing(), mesh_key,
        retired_bindless_meshes_[(frame_index_ + 1) % kFramesInFlight]);
    if (raytracing_) {
      raytracing_->RemoveBlas(mesh_key);
      raytracing_->RemoveApproxBlas(mesh_key);
      raytracing_->RemoveLodBlas(mesh_key);
    }
    if (bindless_ && previous->bindless_geometry)
      bindless_->ReleaseMesh(previous->bindless_index);
    for (gpu::GpuMesh::LodRt &rt : previous->lod_rt) {
      if (rt.indices)
        device_->DestroyBuffer(rt.indices);
      // Now that mesh-table slots recycle, the per-LOD and approx records must
      // come back too or every same-key re-upload leaks table entries.
      if (bindless_ && rt.bindless != BindlessRegistry::kInvalidIndex)
        bindless_->ReleaseMesh(rt.bindless);
    }
    if (bindless_ && previous->rt_approx_bindless_valid)
      bindless_->ReleaseMesh(previous->rt_approx_bindless);
    device_->DestroyBuffer(previous->vertices);
    device_->DestroyBuffer(previous->indices);
    if (previous->skinning)
      device_->DestroyBuffer(previous->skinning);
    if (previous->morph_deltas)
      device_->DestroyBuffer(previous->morph_deltas);
    if (previous->meshlets)
      device_->DestroyBuffer(previous->meshlets);
    if (previous->meshlet_vertices)
      device_->DestroyBuffer(previous->meshlet_vertices);
    if (previous->meshlet_triangles)
      device_->DestroyBuffer(previous->meshlet_triangles);
    if (previous->rt_approx_vertices)
      device_->DestroyBuffer(previous->rt_approx_vertices);
    if (previous->rt_approx_indices)
      device_->DestroyBuffer(previous->rt_approx_indices);
  }
  meshes_[mesh_key] = gpu;
  instances_.RefreshMesh(*device_, mesh_key, gpu.bounds_center,
                         gpu.bounds_radius,
                         SupportsStaticInstances(gpu, material_system_.Get_UseOnlyIfYouKnowWhatYouareDoing()) &&
                             mesh.emitters.empty());
  // NIF particle emitters ride along with the mesh; every placed draw of it
  // feeds a cpu pool (see emitter_sim_ in BuildFrameGraph).
  if (!mesh.emitters.empty())
    register_emitters(mesh.emitters);
  else
    mesh_emitters_.erase(mesh_key);
  // Pure transparency never enters the tlas: water occluding rtao and
  // shadow rays would black out everything under it.
  if (raytracing_ && !gpu.all_blend && include_rt && gpu.bindless_geometry &&
      !raytracing_->BuildBlas(mesh_key, gpu)) {
    bindless_->ReleaseMesh(gpu.bindless_index);
    gpu.bindless_index = 0;
    gpu.bindless_geometry = false;
    if (gpu::GpuMesh *stored = meshes_.find(mesh_key)) {
      stored->bindless_index = 0;
      stored->bindless_geometry = false;
    }
    rt_geometry_dirty_ = true;
  }
  // Opaque-approximation BLAS for the shrunk vegetation stand-in (see above).
  if (raytracing_ && gpu.rt_approx && !approx_accel.empty())
    raytracing_->BuildApproxBlas(mesh_key, approx_accel);
  // SDF: generate a per-mesh signed distance field from the lod-0 CPU geometry.
  // The SDF stands in for RCGI's realtime visibility rays
  // (RX_RAY_MASK_REALTIME), so it must mirror that TLAS set exactly: skip no_rt
  // fill geometry entirely (never in the realtime tlas), and build the field
  // from only the OPAQUE submesh triangle ranges (blended submeshes, i.e.
  // glass/water/effects, are excluded from the tlas and must not turn the SDF
  // opaque). Average albedo / emissive over the opaque submeshes only, matching
  // the geometry that fed it. Eligibility can flip on a same-key re-upload
  // (opaque mesh replaced by an all- blend / no_rt one, or one that lost its
  // opaque submeshes). When it does the block below never calls RegisterMesh,
  // so the previous field must be dropped explicitly or it lingers as a stale
  // occluder; Remove covers every such replacement path (no-op when nothing
  // was registered).
  bool sdf_eligible =
      sdf_scene_ && !gpu.all_blend && !gpu.no_rt && !gpu.dynamic_vertices;
  if (sdf_eligible) {
    SdfScene::MeshInput in{};
    in.positions = lod.vertices.empty() ? nullptr : lod.vertices[0].position;
    in.position_stride = static_cast<u32>(sizeof(asset::Vertex));
    in.vertex_count = static_cast<u32>(lod.vertices.size());
    // Concatenate the opaque submeshes' index ranges (their index_offset is
    // absolute in the shared buffer; lod 0's base is 0, so it indexes
    // lod.indices).
    base::Vector<u32> opaque_indices;
    f32 albedo[3] = {0, 0, 0}, emissive[3] = {0, 0, 0};
    u64 weight = 0;
    for (const gpu::GpuSubmesh &sm : gpu.submeshes) {
      if (sm.blend || sm.index_count == 0)
        continue;
      if (!lod.indices.empty()) {
        for (u32 e = 0; e < sm.index_count; ++e) {
          u32 gi = sm.index_offset + e;
          if (gi < lod.indices.size())
            opaque_indices.push_back(lod.indices[gi]);
        }
      }
      if (material_system_) {
        MaterialSystem::MaterialColor mc =
            material_system_->material_color(sm.material);
        u64 w = rx::Max<u64>(sm.index_count, 1);
        for (int k = 0; k < 3; ++k) {
          albedo[k] += mc.albedo[k] * static_cast<f32>(w);
          emissive[k] += mc.emissive[k] * static_cast<f32>(w);
        }
        weight += w;
      }
    }
    in.indices = opaque_indices.empty() ? nullptr : opaque_indices.data();
    in.index_count = static_cast<u32>(opaque_indices.size());
    if (weight > 0) {
      for (int k = 0; k < 3; ++k) {
        in.albedo[k] = albedo[k] / static_cast<f32>(weight);
        in.emissive[k] = emissive[k] / static_cast<f32>(weight);
      }
    }
    // Only register when there is opaque indexed geometry to voxelise (an all-
    // blend mesh is already excluded above; a non-indexed opaque mesh is not a
    // shape we generate SDFs for here). If there is none, this is not eligible
    // after all, drop any prior field below.
    if (in.positions && in.index_count > 0)
      sdf_scene_->RegisterMesh(mesh_key, in);
    else
      sdf_eligible = false;
  }
  // Any ineligible replacement (or first upload) removes a stale SDF for this
  // key.
  if (sdf_scene_ && !sdf_eligible)
    sdf_scene_->Remove(mesh_key);
  // Foliage uploaded while path tracing was off got no blas/geometry above;
  // flag a catch-up so toggling path tracing on later still pulls it into the
  // tlas (BuildFrameGraph runs EnsureRayTracingGeometry on the next path-traced
  // frame).
  if (raytracing_ && !gpu.all_blend && gpu.no_rt && !include_rt)
    rt_foliage_dirty_ = true;
  if (replacing_mesh)
    ++scene_revision_;
  return true;
}

bool Renderer::UpdateDynamicMesh(const asset::Mesh &mesh, u64 id_salt) {
  if (!device_ || device_->is_stub() || !mesh.dynamic_vertices ||
      mesh.lods.size() != 1) {
    return false;
  }
  const asset::MeshLod &lod = mesh.lods[0];
  const u64 key = mesh.id.hash ^ id_salt;
  gpu::GpuMesh *gpu = meshes_.find(key);
  // rt_approx meshes are rejected: the opaque-approx stand-in duplicates the
  // masked geometry into its own buffers/BLAS, which this fast path does not
  // rebuild; realtime rays would keep hitting the pre-edit shape. Callers
  // fall back to a full UploadMesh, which rebuilds the stand-in.
  if (!gpu || gpu->skinned || gpu->morph_target_count != 0 ||
      !gpu->lods.empty() || gpu->rt_approx ||
      lod.vertices.size() != gpu->vertex_count ||
      lod.indices.size() != gpu->index_count) {
    return false;
  }

  const ByteSpan bytes(reinterpret_cast<const u8 *>(lod.vertices.data()),
                       lod.vertices.size() * sizeof(asset::Vertex));
  const gpu::BufferUsageFlags rt_usage =
      raytracing_ ? (gpu::kBufferUsageAccelBuildInput | gpu::kBufferUsageDeviceAddress |
                     gpu::kBufferUsageStorage)
                  : 0;
  gpu::GpuBuffer replacement =
      device_->CreateBufferWithData(bytes, gpu::kBufferUsageVertex | rt_usage);
  if (!replacement)
    return false;

  if (gpu->bindless_geometry) {
    retired_bindless_meshes_[(frame_index_ + 1) % kFramesInFlight].push_back(
        gpu->bindless_index);
    gpu->bindless_index = 0;
    gpu->bindless_geometry = false;
    if (gpu->no_rt)
      rt_foliage_dirty_ = true;
  }
  if (raytracing_)
    raytracing_->RemoveBlasDeferred(key);
  gpu::GpuBuffer previous = gpu->vertices;
  gpu->vertices = replacement;
  base::MemCopy(gpu->bounds_center, mesh.bounds_center,
              sizeof(gpu->bounds_center));
  gpu->bounds_radius = mesh.bounds_radius;
  device_->DestroyBufferDeferred(previous);
  if (!gpu->dynamic_vertices) {
    if (gpu->meshlets)
      device_->DestroyBufferDeferred(gpu->meshlets);
    if (gpu->meshlet_vertices)
      device_->DestroyBufferDeferred(gpu->meshlet_vertices);
    if (gpu->meshlet_triangles)
      device_->DestroyBufferDeferred(gpu->meshlet_triangles);
    gpu->meshlets = {};
    gpu->meshlet_vertices = {};
    gpu->meshlet_triangles = {};
    gpu->has_meshlets = false;
    gpu->lods.clear();
    gpu->dynamic_vertices = true;
    if (sdf_scene_)
      sdf_scene_->RemoveDeferred(key);
  }
  instances_.RefreshMesh(
      *device_, mesh.id.hash ^ id_salt, gpu->bounds_center, gpu->bounds_radius,
      SupportsStaticInstances(*gpu, material_system_.Get_UseOnlyIfYouKnowWhatYouareDoing()) &&
          mesh.emitters.empty());
  ++scene_revision_;
  return true;
}

bool Renderer::SyncDynamicMeshRayTracing(const asset::Mesh &mesh, u64 id_salt) {
  if (!device_ || device_->is_stub())
    return false;
  const u64 key = mesh.id.hash ^ id_salt;
  gpu::GpuMesh *gpu = meshes_.find(key);
  if (!gpu || !gpu->dynamic_vertices)
    return false;
  if (!raytracing_ || !bindless_ || !material_system_ ||
      (gpu->no_rt && !settings_.path_trace)) {
    return true;
  }
  if (gpu->bindless_geometry && raytracing_->HasBlas(key))
    return true;

  base::Vector<BindlessRegistry::GeometryRecord> geometries;
  for (const gpu::GpuSubmesh &submesh : gpu->submeshes) {
    if (submesh.blend || submesh.index_count == 0)
      continue;
    geometries.push_back(
        {submesh.index_offset,
         material_system_->bindless_material(submesh.material)});
  }
  const u32 bindless_index =
      bindless_->RegisterMesh(gpu->vertices, gpu->indices, geometries.data(),
                              static_cast<u32>(geometries.size()));
  if (bindless_index == BindlessRegistry::kInvalidIndex) {
    rt_geometry_dirty_ = true;
    if (gpu->no_rt)
      rt_foliage_dirty_ = true;
    return false;
  }
  gpu->bindless_index = bindless_index;
  gpu->bindless_geometry = true;
  if (!raytracing_->BuildBlas(key, *gpu)) {
    bindless_->ReleaseMesh(bindless_index);
    gpu->bindless_index = 0;
    gpu->bindless_geometry = false;
    rt_geometry_dirty_ = true;
    return false;
  }
  ++scene_revision_;
  return true;
}

bool Renderer::RemoveDynamicMesh(asset::AssetId mesh, u64 id_salt) {
  if (!device_ || device_->is_stub())
    return false;
  const u64 key = mesh.hash ^ id_salt;
  gpu::GpuMesh *gpu = meshes_.find(key);
  if (!gpu || !gpu->dynamic_vertices || gpu->has_meshlets) {
    return false;
  }
  instances_.RefreshMesh(*device_, key, gpu->bounds_center, gpu->bounds_radius,
                         false);
  if (bindless_ && gpu->bindless_geometry) {
    retired_bindless_meshes_[(frame_index_ + 1) % kFramesInFlight].push_back(
        gpu->bindless_index);
  }
  device_->DestroyBufferDeferred(gpu->vertices);
  device_->DestroyBufferDeferred(gpu->indices);
  if (gpu->skinning)
    device_->DestroyBufferDeferred(gpu->skinning);
  if (gpu->morph_deltas)
    device_->DestroyBufferDeferred(gpu->morph_deltas);
  // Per-LOD RT and opaque-approx side state: without these a later UploadMesh
  // under the same asset id would find (and silently reuse) the stale
  // approx/LOD BLAS entries, and the bindless records would leak for good.
  for (gpu::GpuMesh::LodRt &rt : gpu->lod_rt) {
    if (rt.indices)
      device_->DestroyBufferDeferred(rt.indices);
    if (bindless_ && rt.bindless != BindlessRegistry::kInvalidIndex) {
      retired_bindless_meshes_[(frame_index_ + 1) % kFramesInFlight].push_back(
          rt.bindless);
    }
  }
  if (gpu->rt_approx_vertices)
    device_->DestroyBufferDeferred(gpu->rt_approx_vertices);
  if (gpu->rt_approx_indices)
    device_->DestroyBufferDeferred(gpu->rt_approx_indices);
  if (bindless_ && gpu->rt_approx_bindless_valid) {
    retired_bindless_meshes_[(frame_index_ + 1) % kFramesInFlight].push_back(
        gpu->rt_approx_bindless);
  }
  mesh_emitters_.erase(key);
  // A dynamic mesh can never have a registered SDF (sdf_eligible excludes
  // dynamic_vertices), but keep the frame-safe variant so that stays true by
  // construction rather than by argument.
  if (sdf_scene_)
    sdf_scene_->RemoveDeferred(key);
  if (raytracing_) {
    raytracing_->RemoveBlasDeferred(key);
    raytracing_->RemoveApproxBlasDeferred(key);
    raytracing_->RemoveLodBlasDeferred(key);
  }
  meshes_.erase(key);
  skinned_rt_.InvalidateMesh(
      *device_, raytracing_.Get_UseOnlyIfYouKnowWhatYouareDoing(), key,
      retired_bindless_meshes_[(frame_index_ + 1) % kFramesInFlight]);
  ++scene_revision_;
  return true;
}

bool Renderer::EnsureRayTracingGeometry() {
  if (!bindless_ || !raytracing_ || !material_system_)
    return false;
  bool success = true;
  for (auto entry : meshes_) {
    gpu::GpuMesh &gpu = entry.value;
    if (gpu.all_blend || (gpu.no_rt && !settings_.path_trace))
      continue;
    if (raytracing_->HasBlas(entry.key))
      continue; // already built
    base::Vector<BindlessRegistry::GeometryRecord> geometries;
    for (const gpu::GpuSubmesh &submesh : gpu.submeshes) {
      if (submesh.blend || submesh.index_count == 0)
        continue;
      geometries.push_back(
          {submesh.index_offset,
           material_system_->bindless_material(submesh.material)});
    }
    const u32 bindless_index =
        bindless_->RegisterMesh(gpu.vertices, gpu.indices, geometries.data(),
                                static_cast<u32>(geometries.size()));
    if (bindless_index == BindlessRegistry::kInvalidIndex) {
      success = false;
      continue;
    }
    gpu.bindless_index = bindless_index;
    gpu.bindless_geometry = true;
    if (!raytracing_->BuildBlas(entry.key, gpu)) {
      bindless_->ReleaseMesh(bindless_index);
      gpu.bindless_index = 0;
      gpu.bindless_geometry = false;
      success = false;
    }
  }
  return success;
}

u32 Renderer::EnsureLodRtGeometry(u64 mesh_key, gpu::GpuMesh &mesh, u32 lod) {
  if (lod == 0 || lod > mesh.lod_rt.size())
    return BindlessRegistry::kInvalidIndex;
  gpu::GpuMesh::LodRt &rt = mesh.lod_rt[lod - 1];
  if (rt.bindless == BindlessRegistry::kInvalidIndex || rt.geoms.empty())
    return BindlessRegistry::kInvalidIndex; // no RT geometry at this LOD
  if (!rt.blas_built) {
    // Reconstruct the accel geometry from the eagerly-built (absolute-indexed,
    // force-opaque) LOD index buffer and build the BLAS once. The build blocks
    // (ImmediateSubmit), but only the first time this LOD is needed.
    base::Vector<gpu::AccelTriangles> geometries;
    geometries.reserve(rt.geoms.size());
    for (const gpu::GpuMesh::LodRt::Geom &g : rt.geoms) {
      geometries.push_back(
          {.vertex_address = mesh.vertices.address,
           .vertex_stride = sizeof(asset::Vertex),
           .vertex_count = rt.vertex_count,
           .vertex_format = gpu::Format::kRGB32Float,
           .index_address = rt.indices.address + g.index_offset * sizeof(u32),
           .index_count = g.index_count,
           .index_type = gpu::IndexType::kUint32,
           .opaque = true});
    }
    if (!raytracing_->BuildLodBlas(mesh_key, lod, geometries))
      return BindlessRegistry::kInvalidIndex;
    rt.blas_built = true;
  }
  return rt.bindless;
}

void Renderer::SetDecalAtlas(asset::AssetId texture,
                             asset::AssetId normal_atlas) {
  if (!material_system_)
    return;
  // The cached views below dangle if the streamer ever swaps these images.
  material_system_->Pin(texture.hash);
  if (normal_atlas)
    material_system_->Pin(normal_atlas.hash);
  const gpu::GpuImage *img = material_system_->find_texture(texture.hash);
  decal_atlas_view_ = img ? img->view : gpu::TextureView{};
  const gpu::GpuImage *normal_img =
      normal_atlas ? material_system_->find_texture(normal_atlas.hash)
                   : nullptr;
  decal_normal_atlas_view_ = normal_img ? normal_img->view : gpu::TextureView{};
}

u32 Renderer::AcquireSkinnedRt() { return skinned_rt_.Acquire(); }

void Renderer::ReleaseSkinnedRt(u32 actor) {
  if (!device_)
    return;
  skinned_rt_.Release(*device_, raytracing_.Get_UseOnlyIfYouKnowWhatYouareDoing(), actor,
                      retired_bindless_meshes_[(frame_index_ + 1) % kFramesInFlight]);
}

u32 Renderer::AcquireDecalReceiver() { return decal_baker_.AcquireReceiver(); }

void Renderer::ReleaseDecalReceiver(u32 receiver) {
  decal_baker_.ReleaseReceiver(receiver);
}

bool Renderer::StampDecal(const DecalStamp &stamp) {
  return decal_baker_.Stamp(stamp);
}

void Renderer::SetDecalReceiverUv(u32 receiver, f32 scale_u, f32 scale_v,
                                  f32 bias_u, f32 bias_v) {
  decal_baker_.SetReceiverUv(receiver, scale_u, scale_v, bias_u, bias_v);
}

void Renderer::ClearDecals(u32 receiver) { decal_baker_.ClearReceiver(receiver); }

bool Renderer::UploadTexture(const asset::Texture &texture, u64 id_salt) {
  if (!material_system_)
    return false;
  return material_system_->UploadTexture(texture, id_salt);
}

void Renderer::BeginUploadBatch() {
  if (device_)
    device_->BeginUploadBatch();
}

void Renderer::FlushUploadBatch() {
  if (device_)
    device_->FlushUploadBatch();
}

bool Renderer::UpdateMaterial(const asset::Material &material, u64 id_salt) {
  if (!material_system_ || !material_system_->UpdateMaterialParams(material, id_salt))
    return false;
  ++scene_revision_;
  return true;
}

bool Renderer::UploadMaterial(const asset::Material &material, u64 id_salt) {
  if (!material_system_)
    return false;
  return material_system_->UploadMaterial(material, id_salt);
}

} // namespace rx::render
