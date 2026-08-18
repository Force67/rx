#include "asset/usd_loader.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <string>

#include "core/log.h"

#if defined(RX_HAVE_USD)
#include <filesystem>
#include <vector>

#include "asset/asset_id.h"

#include <asset-resolution.hh>
#include <composition.hh>
#include <io-util.hh>
#include <layer.hh>
#include <tinyusdz.hh>
#include <tydra/render-data-converter.hh>
#include <tydra/render-data.hh>

// Implementation lives in third_party/stb_impl.c (rx::stb_impl).
#include <stb_image.h>
#endif

namespace rx::asset {

bool IsUsdPath(std::string_view path) {
  const size_t dot = path.find_last_of('.');
  if (dot == std::string_view::npos)
    return false;
  std::string ext(path.substr(dot));
  for (char &c : ext)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext == ".usd" || ext == ".usda" || ext == ".usdc" || ext == ".usdz";
}

#if !defined(RX_HAVE_USD)

bool LoadUsdScene(const std::string &path, ImportedScene *,
                  const UsdLoadOptions &) {
  RX_ERROR("usd {}: this build has no USD support, reconfigure with RX_USD=ON",
           path);
  return false;
}

#else

namespace {
namespace tt = tinyusdz::tydra;

AssetId ScopedId(const std::string &path, const char *kind, size_t index) {
  return MakeAssetId(path + "#" + kind + std::to_string(index));
}

// Tydra decodes images into RenderScene::buffers keeping the source channel
// count; the engine uploads rgba8. A single-channel source replicates into rgb
// so a greyscale roughness map reads the same through .g as a packed one.
bool ConvertImage(const tt::RenderScene &scene, const tt::TextureImage &image,
                  Texture *out) {
  if (!image.decoded || image.buffer_id < 0 ||
      static_cast<size_t>(image.buffer_id) >= scene.buffers.size())
    return false;
  if (image.width <= 0 || image.height <= 0 || image.channels <= 0 ||
      image.channels > 4)
    return false;

  const tt::BufferData &buffer = scene.buffers[static_cast<size_t>(image.buffer_id)];
  if (buffer.componentType != tt::ComponentType::UInt8)
    return false;

  const size_t texels = static_cast<size_t>(image.width) *
                        static_cast<size_t>(image.height);
  const size_t channels = static_cast<size_t>(image.channels);
  if (buffer.data.size() < texels * channels)
    return false;

  out->format = TextureFormat::kRgba8;
  out->width = static_cast<u32>(image.width);
  out->height = static_cast<u32>(image.height);
  out->is_srgb = image.colorSpace == tt::ColorSpace::sRGB ||
                 image.colorSpace == tt::ColorSpace::sRGB_Texture;
  out->data.resize(texels * 4);

  for (size_t t = 0; t < texels; ++t) {
    const u8 *s = &buffer.data[t * channels];
    u8 *d = &out->data[t * 4];
    switch (channels) {
    case 1: // grey
      d[0] = d[1] = d[2] = s[0];
      d[3] = 255;
      break;
    case 2: // grey + alpha
      d[0] = d[1] = d[2] = s[0];
      d[3] = s[1];
      break;
    case 3:
      d[0] = s[0];
      d[1] = s[1];
      d[2] = s[2];
      d[3] = 255;
      break;
    default:
      std::memcpy(d, s, 4);
      break;
    }
  }
  return true;
}

// UsdPreviewSurface binds a texture through a UsdUVTexture prim: follow the
// shader input's texture id to the UVTexture, then to the decoded image.
i64 ImageIndexOf(const tt::RenderScene &scene, i32 texture_id) {
  if (texture_id < 0 || static_cast<size_t>(texture_id) >= scene.textures.size())
    return -1;
  const i64 image = scene.textures[static_cast<size_t>(texture_id)].texture_image_id;
  if (image < 0 || static_cast<size_t>(image) >= scene.images.size())
    return -1;
  return image;
}

AssetId TextureAssetOf(const tt::RenderScene &scene,
                       const base::Vector<AssetId> &image_ids, i32 texture_id) {
  const i64 image = ImageIndexOf(scene, texture_id);
  return image >= 0 ? image_ids[static_cast<u32>(image)] : AssetId{};
}

// Neither UsdPreviewSurface nor OpenPBR has a packed ORM channel: roughness and
// metallic are separate inputs. When both resolve to one image the author
// packed them glTF-style and the combined path applies; otherwise the roughness
// map goes in the combined slot (read through .g, which the greyscale expansion
// above keeps valid) and metallic gets its own.
void ConvertRoughnessMetallicMaps(const tt::RenderScene &scene,
                                  const base::Vector<AssetId> &image_ids,
                                  i32 roughness_texture, i32 metallic_texture,
                                  Material *out) {
  const i64 roughness_image = ImageIndexOf(scene, roughness_texture);
  const i64 metallic_image = ImageIndexOf(scene, metallic_texture);
  if (roughness_image >= 0)
    out->metallic_roughness = image_ids[static_cast<u32>(roughness_image)];
  if (metallic_image >= 0 && metallic_image != roughness_image) {
    out->metallic_map = image_ids[static_cast<u32>(metallic_image)];
    out->separate_metallic = true;
    if (roughness_image < 0)
      out->metallic_roughness = out->metallic_map;
  } else if (metallic_image >= 0 && roughness_image < 0) {
    out->metallic_roughness = image_ids[static_cast<u32>(metallic_image)];
  } else if (roughness_image >= 0) {
    // Roughness map, no metallic map. The combined slot is roughness-only, but
    // the glTF packing the shader assumes reads metallic from its `.b` - and a
    // greyscale roughness map expands to rgb, so `.b` is the roughness value.
    // Matte cloth with a non-zero `metallic` scalar then shades as tinted
    // metal. kFlagSeparateMetallic switches the shader to metallic_map.r, whose
    // white default makes metallic == metallic_factor: exactly the
    // UsdPreviewSurface meaning of a scalar metallic with no texture.
    out->separate_metallic = true;
  }
}

void ConvertPreviewSurface(const tt::RenderScene &scene,
                           const tt::PreviewSurfaceShader &s,
                           const base::Vector<AssetId> &image_ids, Material *out) {
  const auto texture = [&](i32 texture_id) {
    return TextureAssetOf(scene, image_ids, texture_id);
  };

  out->base_color = texture(s.diffuseColor.texture_id);
  out->base_color_factor[0] = s.diffuseColor.value[0];
  out->base_color_factor[1] = s.diffuseColor.value[1];
  out->base_color_factor[2] = s.diffuseColor.value[2];
  out->base_color_factor[3] = s.opacity.value;

  out->normal = texture(s.normal.texture_id);
  out->emissive = texture(s.emissiveColor.texture_id);
  out->emissive_factor[0] = s.emissiveColor.value[0];
  out->emissive_factor[1] = s.emissiveColor.value[1];
  out->emissive_factor[2] = s.emissiveColor.value[2];

  out->metallic_factor = s.metallic.value;
  out->roughness_factor = s.roughness.value;
  out->ior = s.ior.value;
  out->clearcoat = s.clearcoat.value;
  out->clearcoat_roughness = s.clearcoatRoughness.value;
  out->occlusion_map = texture(s.occlusion.texture_id);

  ConvertRoughnessMetallicMaps(scene, image_ids, s.roughness.texture_id,
                               s.metallic.texture_id, out);
}

// OpenPBR Surface (AcademySoftwareFoundation/OpenPBR v1.1.1). Tydra parses the
// whole MaterialX input set into RenderMaterial::openPBRShader independently of
// UsdPreviewSurface - a stage may carry either, or both - so this is a separate
// mapping rather than a patch over the preview-surface one. Only the lobes the
// engine can actually shade are mapped; the translucent-base volumetrics,
// dispersion, thin-walled mode and separate coat normals are dropped. See
// docs/OPENPBR.md for the full coverage table.
//
// Two things worth knowing about the tydra boundary:
//   - ShaderParam carries no "was this authored" bit, only a value and a
//     texture id. Some of tydra's fallbacks disagree with the spec
//     (coat_roughness 0.1 vs 0, subsurface_radius_scale (1,0.2,0.1) vs
//     (1,0.5,0.25), thin_film_ior 1.5 vs 1.4). An unauthored input is
//     indistinguishable from one authored to the same value, so those are
//     taken as given rather than "corrected" - clobbering a real authored
//     value would be the worse failure.
//   - thin_film_thickness is passed through verbatim from the document, which
//     the spec defines in micrometers, into a field tydra documents as
//     nanometers. The conversion has to happen somewhere, so it happens here.
void ConvertOpenPbrSurface(const tt::RenderScene &scene,
                           const tt::OpenPBRSurfaceShader &s,
                           const base::Vector<AssetId> &image_ids, Material *out) {
  const auto texture = [&](i32 texture_id) {
    return TextureAssetOf(scene, image_ids, texture_id);
  };

  // base_weight scales base_color, which serves as both the diffuse albedo and
  // the metal normal-incidence reflectivity F0.
  const f32 base_weight = s.base_weight.value;
  out->base_color = texture(s.base_color.texture_id);
  out->base_color_factor[0] = s.base_color.value[0] * base_weight;
  out->base_color_factor[1] = s.base_color.value[1] * base_weight;
  out->base_color_factor[2] = s.base_color.value[2] * base_weight;
  out->base_color_factor[3] = s.opacity.value;
  out->base_diffuse_roughness = s.base_diffuse_roughness.value;
  out->metallic_factor = s.base_metalness.value;

  out->normal = texture(s.normal.texture_id);
  ConvertRoughnessMetallicMaps(scene, image_ids, s.specular_roughness.texture_id,
                               s.base_metalness.texture_id, out);

  out->roughness_factor = s.specular_roughness.value;
  out->ior = s.specular_ior.value;
  out->specular_weight = s.specular_weight.value;
  out->specular_color[0] = s.specular_color.value[0];
  out->specular_color[1] = s.specular_color.value[1];
  out->specular_color[2] = s.specular_color.value[2];

  out->anisotropy = OpenPbrAnisotropyToEngine(s.specular_roughness_anisotropy.value);

  out->transmission = s.transmission_weight.value;
  out->subsurface = s.subsurface_weight.value;
  out->subsurface_color[0] = s.subsurface_color.value[0];
  out->subsurface_color[1] = s.subsurface_color.value[1];
  out->subsurface_color[2] = s.subsurface_color.value[2];

  // Fuzz folds onto the engine's Charlie sheen lobe (the spec asks for a
  // Zeltner microflake LTC, which the engine does not implement). Tydra carries
  // both the OpenPBR fuzz_* inputs and the Autodesk standard_surface sheen_*
  // ones on the same struct, so take whichever the document actually drove.
  const bool has_fuzz = s.fuzz_weight.value > 0.0f;
  const f32 sheen_weight = has_fuzz ? s.fuzz_weight.value : s.sheen_weight.value;
  const auto &sheen_tint = has_fuzz ? s.fuzz_color.value : s.sheen_color.value;
  out->sheen_color[0] = sheen_tint[0] * sheen_weight;
  out->sheen_color[1] = sheen_tint[1] * sheen_weight;
  out->sheen_color[2] = sheen_tint[2] * sheen_weight;
  out->sheen_roughness = has_fuzz ? s.fuzz_roughness.value : s.sheen_roughness.value;

  out->clearcoat = s.coat_weight.value;
  out->clearcoat_roughness = s.coat_roughness.value;
  out->coat_ior = s.coat_ior.value;
  out->coat_darkening = s.coat_darkening.value;
  out->coat_color[0] = s.coat_color.value[0];
  out->coat_color[1] = s.coat_color.value[1];
  out->coat_color[2] = s.coat_color.value[2];

  out->iridescence = s.thin_film_weight.value;
  out->thin_film_ior = s.thin_film_ior.value;
  if (out->iridescence > 0.0f) {
    // Micrometers in the document, nanometers in the engine. A zero thickness
    // with the film switched on is tydra's unauthored fallback rather than a
    // real "no film", so fall back to the spec default of 0.5um.
    const f32 thickness_um =
        s.thin_film_thickness.value > 0.0f ? s.thin_film_thickness.value : 0.5f;
    out->iridescence_thickness = thickness_um * 1000.0f;
  }

  // emission_luminance is an absolute luminance in nits, and emission_color a
  // (possibly HDR) multiplier on it. The engine's emissive_factor is a linear
  // radiance addend, so this is passed through literally and left to the auto
  // exposure to resolve rather than rescaled by an invented constant.
  const f32 emission = s.emission_luminance.value;
  out->emissive = texture(s.emission_color.texture_id);
  out->emissive_factor[0] = s.emission_color.value[0] * emission;
  out->emissive_factor[1] = s.emission_color.value[1] * emission;
  out->emissive_factor[2] = s.emission_color.value[2] * emission;
}

void ConvertMaterial(const tt::RenderScene &scene, const tt::RenderMaterial &src,
                     const base::Vector<AssetId> &image_ids, Material *out) {
  out->name = src.name;
  // A material can carry both shaders. OpenPBR is the richer model, so it wins.
  if (src.openPBRShader) {
    ConvertOpenPbrSurface(scene, *src.openPBRShader, image_ids, out);
  } else if (src.surfaceShader) {
    ConvertPreviewSurface(scene, *src.surfaceShader, image_ids, out);
  } else {
    return; // MDL-only material: keep the engine defaults.
  }

  switch (src.materialTag) {
  case tt::MaterialTag::Masked:
    out->alpha_mode = AlphaMode::kMask;
    // OpenPBR has no opacity-threshold input; its cutout is geometry_opacity
    // driven by a mask texture, so the engine's own default cutoff stands.
    if (src.surfaceShader && !src.openPBRShader)
      out->alpha_cutoff = src.surfaceShader->opacityThreshold.value;
    break;
  case tt::MaterialTag::Translucent:
    out->alpha_mode = AlphaMode::kBlend;
    break;
  case tt::MaterialTag::Opaque:
    out->alpha_mode = AlphaMode::kOpaque;
    break;
  }
}

// A tydra VertexAttribute is a raw byte blob plus a format; every attribute we
// read back is float-based and, with build_vertex_indices on, vertex-varying.
const f32 *AttributeFloats(const tt::VertexAttribute &attribute,
                           size_t expected_vertices, size_t components) {
  if (attribute.empty() || !attribute.is_vertex())
    return nullptr;
  if (attribute.format_size() != components * sizeof(f32))
    return nullptr;
  if (attribute.vertex_count() != expected_vertices)
    return nullptr;
  return static_cast<const f32 *>(attribute.buffer());
}

bool ConvertMesh(const tt::RenderMesh &src,
                 const base::Vector<AssetId> &material_ids, Mesh *out) {
  const size_t vertex_count = src.points.size();
  const std::vector<u32> &indices = src.faceVertexIndices();
  if (vertex_count == 0 || indices.empty())
    return false;
  // build_vertex_indices makes every attribute share the point index buffer.
  // Without it normals/uvs are facevarying and would need their own indices.
  if (!src.is_single_indexable || !src.is_triangulated())
    return false;

  const f32 *normals = AttributeFloats(src.normals, vertex_count, 3);
  const f32 *tangents = AttributeFloats(src.tangents, vertex_count, 3);
  const f32 *binormals = AttributeFloats(src.binormals, vertex_count, 3);
  const f32 *colors = AttributeFloats(src.vertex_colors, vertex_count, 3);
  const f32 *opacities = AttributeFloats(src.vertex_opacities, vertex_count, 1);
  // `primvars:displayColor` is Hydra's *preview* color: Storm shades with it
  // only when a prim has no material bound, and the MDL/RTX renderers ignore
  // it outright. The engine multiplies vertex color into base color (the glTF
  // COLOR_0 contract), so baking displayColor under a bound material would
  // tint the whole object with a value the source renderer never used -
  // NVIDIA's Attic authors a leftover constant red (1,0,0) on its wall,
  // window and beam meshes exactly this way. Keep it only as the unlit
  // fallback for materially unbound meshes, and never when it carries the
  // negative "unauthored" sentinel some exporters write.
  bool material_bound = src.material_id >= 0;
  for (const auto &[subset_name, subset] : src.material_subsetMap)
    material_bound = material_bound || subset.material_id >= 0;
  if (colors && (material_bound || colors[0] < 0.0f || colors[1] < 0.0f ||
                 colors[2] < 0.0f)) {
    colors = nullptr;
    opacities = nullptr;
  }
  const f32 *uvs = nullptr;
  if (const auto it = src.texcoords.find(0); it != src.texcoords.end())
    uvs = AttributeFloats(it->second, vertex_count, 2);

  MeshLod lod;
  lod.vertices.resize(static_cast<u32>(vertex_count));
  Vec3 lo{src.points[0][0], src.points[0][1], src.points[0][2]};
  Vec3 hi = lo;
  for (size_t v = 0; v < vertex_count; ++v) {
    Vertex &vertex = lod.vertices[static_cast<u32>(v)];
    vertex.position[0] = src.points[v][0];
    vertex.position[1] = src.points[v][1];
    vertex.position[2] = src.points[v][2];
    lo = {std::min(lo.x, vertex.position[0]), std::min(lo.y, vertex.position[1]),
          std::min(lo.z, vertex.position[2])};
    hi = {std::max(hi.x, vertex.position[0]), std::max(hi.y, vertex.position[1]),
          std::max(hi.z, vertex.position[2])};

    if (normals)
      std::memcpy(vertex.normal, normals + v * 3, 3 * sizeof(f32));
    else
      vertex.normal[1] = 1.0f;

    if (uvs) {
      vertex.uv[0] = uvs[v * 2 + 0];
      // USD texture space has v growing up, the engine samples v down.
      vertex.uv[1] = 1.0f - uvs[v * 2 + 1];
    }

    if (tangents) {
      std::memcpy(vertex.tangent, tangents + v * 3, 3 * sizeof(f32));
      // Handedness the shader needs to rebuild the bitangent: compare the
      // authored binormal against the one the TBN would generate.
      f32 w = 1.0f;
      if (binormals && normals) {
        const f32 *n = normals + v * 3;
        const f32 *t = tangents + v * 3;
        const f32 *b = binormals + v * 3;
        const f32 cross[3] = {n[1] * t[2] - n[2] * t[1],
                              n[2] * t[0] - n[0] * t[2],
                              n[0] * t[1] - n[1] * t[0]};
        if (cross[0] * b[0] + cross[1] * b[1] + cross[2] * b[2] < 0)
          w = -1.0f;
      }
      vertex.tangent[3] = w;
    }

    if (colors) {
      const f32 alpha = opacities ? opacities[v] : 1.0f;
      const auto quantize = [](f32 c) {
        return static_cast<u32>(std::clamp(c, 0.0f, 1.0f) * 255.0f + 0.5f);
      };
      vertex.color = quantize(colors[v * 3 + 0]) |
                     (quantize(colors[v * 3 + 1]) << 8) |
                     (quantize(colors[v * 3 + 2]) << 16) |
                     (quantize(alpha) << 24);
    }
  }

  out->bounds_center[0] = (lo.x + hi.x) * 0.5f;
  out->bounds_center[1] = (lo.y + hi.y) * 0.5f;
  out->bounds_center[2] = (lo.z + hi.z) * 0.5f;
  const Vec3 extent{hi.x - lo.x, hi.y - lo.y, hi.z - lo.z};
  out->bounds_radius =
      0.5f * std::sqrt(extent.x * extent.x + extent.y * extent.y +
                       extent.z * extent.z);

  const auto material_of = [&](int id) -> AssetId {
    return id >= 0 && static_cast<u32>(id) < material_ids.size()
               ? material_ids[static_cast<u32>(id)]
               : AssetId{};
  };
  // USD `orientation = "leftHanded"` reverses the face winding the engine
  // expects; flipping here keeps the raster backface test correct.
  const bool flip = !src.is_rightHanded;
  const size_t triangles = indices.size() / 3;
  const auto emit = [&](size_t triangle) {
    const u32 *tri = &indices[triangle * 3];
    lod.indices.push_back(tri[0]);
    lod.indices.push_back(flip ? tri[2] : tri[1]);
    lod.indices.push_back(flip ? tri[1] : tri[2]);
  };

  lod.indices.reserve(static_cast<u32>(indices.size()));
  if (src.material_subsetMap.empty()) {
    for (size_t t = 0; t < triangles; ++t)
      emit(t);
    lod.submeshes.push_back(Submesh{0, static_cast<u32>(lod.indices.size()),
                                    material_of(src.material_id)});
  } else {
    // A materialBind GeomSubset selects faces, not index ranges, so gather each
    // subset's triangles into one contiguous run and give the faces no subset
    // claims a trailing submesh on the mesh-wide material.
    std::vector<bool> claimed(triangles, false);
    for (const auto &[name, subset] : src.material_subsetMap) {
      Submesh submesh;
      submesh.index_offset = static_cast<u32>(lod.indices.size());
      for (const int face : subset.indices()) {
        if (face < 0 || static_cast<size_t>(face) >= triangles)
          continue;
        emit(static_cast<size_t>(face));
        claimed[static_cast<size_t>(face)] = true;
      }
      submesh.index_count =
          static_cast<u32>(lod.indices.size()) - submesh.index_offset;
      if (submesh.index_count == 0)
        continue;
      submesh.material = material_of(subset.material_id);
      lod.submeshes.push_back(submesh);
    }
    Submesh rest;
    rest.index_offset = static_cast<u32>(lod.indices.size());
    for (size_t t = 0; t < triangles; ++t) {
      if (!claimed[t])
        emit(t);
    }
    rest.index_count = static_cast<u32>(lod.indices.size()) - rest.index_offset;
    if (rest.index_count > 0) {
      rest.material = material_of(src.material_id);
      lod.submeshes.push_back(rest);
    }
  }

  if (lod.submeshes.empty())
    return false;
  out->lods.push_back(std::move(lod));
  return true;
}

// USD stores matrices row-major for row-vector maths (v' = v * M), so row i
// holds basis vector i. The engine is column-major for column vectors
// (v' = M * v), where column i holds basis vector i: the flat element order is
// the same and the copy is a straight widening.
Mat4 ToMat4(const tinyusdz::value::matrix4d &m) {
  Mat4 out;
  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 4; ++col)
      out.m[row * 4 + col] = static_cast<f32>(m.m[row][col]);
  }
  return out;
}

ImportedScene::Instance MakeInstance(u32 mesh_index, const Mat4 &world) {
  ImportedScene::Instance instance;
  instance.mesh_index = mesh_index;
  instance.position = {world.m[12], world.m[13], world.m[14]};
  const auto axis_length = [&](int col) {
    const f32 *c = &world.m[col * 4];
    return std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
  };
  instance.scale = (axis_length(0) + axis_length(1) + axis_length(2)) / 3.0f;
  const Quat rotation = QuatFromMat4(world);
  instance.rotation[0] = rotation.x;
  instance.rotation[1] = rotation.y;
  instance.rotation[2] = rotation.z;
  instance.rotation[3] = rotation.w;
  return instance;
}

struct NodeWalk {
  const tt::RenderScene &scene;
  Mat4 stage_to_engine;
  ImportedScene *out;
  u32 mirrored = 0;

  void Visit(const tt::Node &node) {
    i32 mesh = -1;
    if (node.is_instance && node.instance_id >= 0 &&
        static_cast<size_t>(node.instance_id) < scene.instances.size()) {
      mesh = scene.instances[static_cast<size_t>(node.instance_id)].mesh_id;
    } else if (node.nodeType == tt::NodeType::Mesh) {
      mesh = node.id;
    }

    if (mesh >= 0 && static_cast<u32>(mesh) < out->meshes.size()) {
      const Mat4 world = stage_to_engine * ToMat4(node.global_matrix);
      // A negative determinant means the prim is mirrored, which a
      // position/quaternion/uniform-scale instance cannot express.
      const f32 det =
          world.m[0] * (world.m[5] * world.m[10] - world.m[6] * world.m[9]) -
          world.m[4] * (world.m[1] * world.m[10] - world.m[2] * world.m[9]) +
          world.m[8] * (world.m[1] * world.m[6] - world.m[2] * world.m[5]);
      if (det < 0)
        ++mirrored;
      out->instances.push_back(MakeInstance(static_cast<u32>(mesh), world));
    }

    for (const tt::Node &child : node.children)
      Visit(child);
  }
};

// Resolving one round of arcs can pull in layers that carry arcs of their own,
// so composition runs to a fixed point. The cap only bounds pathological input.
constexpr int kMaxCompositionPasses = 16;

// tinyusdz's stock texture loader refuses any asset path that starts with
// `..`, which is how authored scenes normally point at a shared texture
// directory beside the layer (`@../Materials/Wood_BaseColor.png@`). That policy
// is right for a resolver serving remote or packaged assets; rx is reading the
// local directory the stage came from, so this loader resolves relative to the
// stage instead and decodes with the same stb the glTF loader uses.
bool LoadTextureAsset(const tinyusdz::value::AssetPath &asset_path,
                      const tinyusdz::AssetInfo &,
                      const tinyusdz::AssetResolutionResolver &,
                      tt::TextureImage *out, std::vector<u8> *pixels,
                      void *userdata, std::string *, std::string *err) {
  const auto *base_dir = static_cast<const std::string *>(userdata);
  const std::string asset = asset_path.GetAssetPath();
  if (asset.empty()) {
    if (err)
      *err += "empty texture asset path\n";
    return false;
  }

  // Composition rewrites a referenced layer's asset paths to be reachable from
  // the process working directory, so the path usually resolves as authored;
  // one that does not is layer-relative and needs the stage's directory.
  std::filesystem::path resolved(asset);
  if (!std::filesystem::exists(resolved) && resolved.is_relative() && base_dir &&
      !base_dir->empty()) {
    resolved = std::filesystem::path(*base_dir) / asset;
  }
  if (!std::filesystem::exists(resolved)) {
    if (err)
      *err += "texture not found: " + asset + "\n";
    return false;
  }

  int width = 0, height = 0, channels = 0;
  stbi_uc *decoded =
      stbi_load(resolved.string().c_str(), &width, &height, &channels, 0);
  if (!decoded) {
    if (err)
      *err += "could not decode texture: " + resolved.string() + "\n";
    return false;
  }

  out->asset_identifier = resolved.string();
  out->width = width;
  out->height = height;
  out->channels = channels;
  out->texelComponentType = tt::ComponentType::UInt8;
  out->assetTexelComponentType = tt::ComponentType::UInt8;
  out->decoded = true;
  const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) *
                       static_cast<size_t>(channels);
  pixels->assign(decoded, decoded + bytes);
  stbi_image_free(decoded);
  return true;
}

// Omniverse writes `colorSpace = "RAW"` on texture assets, and tinyusdz only
// matches the spellings USD's own documentation uses ("raw", "Raw", "sRGB",
// ...); an unrecognized token is a hard error that fails the whole stage, not
// just that material. Every token it does accept either is lowercase already or
// lowercases into another one it accepts, so folding case costs nothing.
u32 NormalizeColorSpaceTokens(tinyusdz::PrimSpec &spec) {
  u32 changed = 0;
  for (auto &[name, property] : spec.props()) {
    if (!property.is_attribute())
      continue;
    tinyusdz::AttrMeta &metas = property.attribute().metas();
    if (!metas.has_colorSpace())
      continue;
    const std::string authored = metas.get_colorSpace().str();
    std::string lowered = authored;
    for (char &c : lowered)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lowered != authored) {
      metas.set_colorSpace(lowered);
      ++changed;
    }
  }
  for (tinyusdz::PrimSpec &child : spec.children())
    changed += NormalizeColorSpaceTokens(child);
  return changed;
}

Vec3 TransformDirectionNormalized(const Mat4 &m, const Vec3 &v) {
  Vec3 d = TransformDir(m, v);
  const f32 len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
  if (len > 1e-8f) {
    d.x /= len;
    d.y /= len;
    d.z /= len;
  }
  return d;
}

// A light's texture (dome envmap) is authored relative to the layer that
// declares it, which for a lighting rig kept in a subdirectory is one level
// below the root stage. Try the authored spelling, then the stage directory,
// then the stage directory with the leading parent hops folded away.
std::string ResolveLightTexture(const std::string &asset,
                                const std::string &base_dir) {
  if (asset.empty())
    return {};
  namespace fs = std::filesystem;
  if (fs::exists(asset))
    return asset;
  if (!base_dir.empty()) {
    const fs::path direct = fs::path(base_dir) / asset;
    if (fs::exists(direct))
      return direct.string();
    std::string trimmed = asset;
    while (trimmed.rfind("../", 0) == 0)
      trimmed = trimmed.substr(3);
    const fs::path folded = fs::path(base_dir) / trimmed;
    if (fs::exists(folded))
      return folded.string();
  }
  return {};
}

// Solid-angle averaged chromaticity of an equirectangular environment map,
// normalized to unit luminance. Rows are weighted by sin(theta) because an
// equirect image oversamples the poles; without that a bright horizon sun reads
// as far less of the average than it really is.
bool AverageEnvmapColor(const std::string &path, f32 out_rgb[3]) {
  int width = 0, height = 0, channels = 0;
  f32 *pixels = stbi_loadf(path.c_str(), &width, &height, &channels, 3);
  if (!pixels)
    return false;

  f64 sum[3] = {0, 0, 0};
  f64 weight_total = 0;
  // A few hundred rows is plenty for an average and keeps a 4k map cheap.
  const int row_step = std::max(1, height / 256);
  const int col_step = std::max(1, width / 512);
  for (int y = 0; y < height; y += row_step) {
    const f64 theta = (static_cast<f64>(y) + 0.5) / height * 3.14159265358979;
    const f64 weight = std::sin(theta);
    for (int x = 0; x < width; x += col_step) {
      const f32 *p = pixels + (static_cast<size_t>(y) * width + x) * 3;
      // Guard against inf/nan, which show up in the wild in exr-sourced hdr.
      for (int c = 0; c < 3; ++c) {
        const f32 v = p[c];
        if (std::isfinite(v) && v > 0.0f) sum[c] += weight * v;
      }
      weight_total += weight;
    }
  }
  stbi_image_free(pixels);
  if (weight_total <= 0)
    return false;

  f64 rgb[3];
  for (int c = 0; c < 3; ++c) rgb[c] = sum[c] / weight_total;
  const f64 luminance = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
  if (!(luminance > 1e-9))
    return false;
  for (int c = 0; c < 3; ++c)
    out_rgb[c] = static_cast<f32>(rgb[c] / luminance);
  return true;
}

// UsdLux moved every light attribute into the `inputs:` namespace in USD
// 21.02. Scenes authored before that - which is most published USD content,
// NVIDIA's own samples included - spell them bare (`intensity`, `color`,
// `texture:file`). tinyusdz binds its typed schema to the modern names only, so
// on a legacy stage every light silently reconstructs as struct defaults: white,
// intensity 1, and the whole authored rig is lost. Renaming here, before the
// stage is built, lets the stock Tydra light conversion see them.
//
// The reverse never needs handling: a modern stage already carries `inputs:`,
// and an already-namespaced property wins over a bare one of the same name.
const char *const kLuxLegacyInputs[] = {
    "intensity",       "exposure",         "diffuse",
    "specular",        "normalize",        "color",
    "enableColorTemperature",              "colorTemperature",
    "radius",          "width",            "height",
    "length",          "angle",            "treatAsPoint",
    "treatAsLine",     "texture:file",     "texture:format",
    "shaping:cone:angle",                  "shaping:cone:softness",
    "shaping:focus",   "shaping:focusTint","shaping:ies:file",
    "shaping:ies:angleScale",              "shaping:ies:normalize",
};

bool IsLightTypeName(const std::string &type_name) {
  // UsdLuxDomeLight, SphereLight, RectLight, DiskLight, CylinderLight,
  // DistantLight, GeometryLight, PortalLight - all end in "Light".
  return type_name.size() > 5 &&
         type_name.compare(type_name.size() - 5, 5, "Light") == 0;
}

// Renames legacy light attributes and records prims hidden by `visibility`.
// Visibility is inherited, so an invisible ancestor hides the whole subtree;
// the Attic keeps a full second lighting rig and a 500-bulb string-light strand
// switched off exactly this way, and importing them would double-light it.
u32 NormalizeLuxSchema(tinyusdz::PrimSpec &spec, const std::string &parent_path,
                       bool parent_hidden,
                       base::Vector<std::string> *hidden_paths) {
  const std::string path = parent_path + "/" + spec.name();

  bool hidden = parent_hidden;
  if (!hidden) {
    auto vis = spec.props().find("visibility");
    if (vis != spec.props().end() && vis->second.is_attribute()) {
      tinyusdz::value::token token;
      if (vis->second.get_attribute().get_value(&token) &&
          token.str() == "invisible") {
        hidden = true;
      }
    }
  }
  if (hidden && !parent_hidden) hidden_paths->push_back(path);

  u32 renamed = 0;
  if (IsLightTypeName(spec.typeName())) {
    for (const char *legacy : kLuxLegacyInputs) {
      auto it = spec.props().find(legacy);
      if (it == spec.props().end()) continue;
      const std::string modern = std::string("inputs:") + legacy;
      if (spec.props().count(modern)) continue; // authored both ways: keep new
      tinyusdz::Property moved = it->second;
      spec.props().erase(it);
      spec.props().emplace(modern, std::move(moved));
      ++renamed;
    }
  }

  for (tinyusdz::PrimSpec &child : spec.children())
    renamed += NormalizeLuxSchema(child, path, hidden, hidden_paths);
  return renamed;
}

bool CoveredBy(const std::string &abs_path,
               const base::Vector<std::string> &prefixes) {
  for (const std::string &prefix : prefixes) {
    if (abs_path.size() >= prefix.size() &&
        abs_path.compare(0, prefix.size(), prefix) == 0 &&
        (abs_path.size() == prefix.size() || abs_path[prefix.size()] == '/'))
      return true;
  }
  return false;
}

bool IsHidden(const std::string &abs_path,
              const base::Vector<std::string> &hidden_paths,
              const UsdLoadOptions &options) {
  if (CoveredBy(abs_path, options.hide)) return true;
  if (CoveredBy(abs_path, options.show)) return false;
  return CoveredBy(abs_path, hidden_paths);
}

void ConvertLights(const tt::RenderScene &scene, const Mat4 &stage_to_engine,
                   f32 meters_per_unit,
                   const base::Vector<std::string> &hidden_paths,
                   const UsdLoadOptions &options, const std::string &base_dir,
                   ImportedScene *out) {
  using Kind = ImportedScene::Light::Kind;
  u32 skipped = 0;
  for (const tt::RenderLight &src : scene.lights) {
    if (IsHidden(src.abs_path, hidden_paths, options)) {
      ++skipped;
      continue;
    }
    ImportedScene::Light light;
    switch (src.type) {
    case tt::RenderLight::Type::Distant: light.kind = Kind::kDistant; break;
    case tt::RenderLight::Type::Dome: light.kind = Kind::kDome; break;
    case tt::RenderLight::Type::Rect: light.kind = Kind::kRect; break;
    case tt::RenderLight::Type::Disk: light.kind = Kind::kDisk; break;
    case tt::RenderLight::Type::Cylinder: light.kind = Kind::kCylinder; break;
    case tt::RenderLight::Type::Point:
    case tt::RenderLight::Type::Sphere: light.kind = Kind::kSphere; break;
    default:
      // Geometry and portal lights have no engine equivalent.
      ++skipped;
      continue;
    }

    light.position = TransformPoint(
        stage_to_engine, {src.position[0], src.position[1], src.position[2]});
    light.direction = TransformDirectionNormalized(
        stage_to_engine, {src.direction[0], src.direction[1], src.direction[2]});
    light.color[0] = src.color[0];
    light.color[1] = src.color[1];
    light.color[2] = src.color[2];
    light.intensity = src.intensity;
    light.exposure = src.exposure;
    // Shape extents are authored in stage units like everything else.
    light.radius = src.radius * meters_per_unit;
    light.width = src.width * meters_per_unit;
    light.height = src.height * meters_per_unit;
    light.length = src.length * meters_per_unit;
    light.cone_angle = src.shapingConeAngle;
    light.cone_softness = src.shapingConeSoftness;
    light.normalize = src.normalize;
    light.texture = ResolveLightTexture(src.textureFile, base_dir);
    if (!light.texture.empty())
      AverageEnvmapColor(light.texture, light.texture_average);

    // UsdLux colorTemperature overrides the authored color when enabled.
    if (src.enableColorTemperature) {
      // Planckian locus, Krystek's rational fit, good to ~1% over 1667-25000K.
      const f32 t = src.colorTemperature;
      const f32 u = (0.860117757f + 1.54118254e-4f * t + 1.28641212e-7f * t * t) /
                    (1.0f + 8.42420235e-4f * t + 7.08145163e-7f * t * t);
      const f32 v = (0.317398726f + 4.22806245e-5f * t + 4.20481691e-8f * t * t) /
                    (1.0f - 2.89741816e-5f * t + 1.61456053e-7f * t * t);
      const f32 d = 2.0f * u - 8.0f * v + 4.0f;
      const f32 x = 3.0f * u / d, y = 2.0f * v / d;
      const f32 Y = 1.0f, X = (y > 1e-6f) ? (x * Y / y) : 0.0f;
      const f32 Z = (y > 1e-6f) ? ((1.0f - x - y) * Y / y) : 0.0f;
      f32 rgb[3] = {3.2406f * X - 1.5372f * Y - 0.4986f * Z,
                    -0.9689f * X + 1.8758f * Y + 0.0415f * Z,
                    0.0557f * X - 0.2040f * Y + 1.0570f * Z};
      f32 peak = std::max(rgb[0], std::max(rgb[1], rgb[2]));
      if (peak <= 0.0f) peak = 1.0f;
      // UsdLux multiplies the blackbody colour into `inputs:color`; it does not
      // replace it (see tinyusdz usdLux.cc GetColorTemperatureRGB callers).
      for (int c = 0; c < 3; ++c)
        light.color[c] *= std::max(0.0f, rgb[c] / peak);
    }

    out->lights.push_back(std::move(light));
  }
  if (skipped)
    RX_DEBUG("usd: skipped {} hidden or unsupported light(s)", skipped);
}

void ConvertCameras(const tt::RenderScene &scene, const Mat4 &stage_to_engine,
                    const base::Vector<std::string> &hidden_paths,
                    const UsdLoadOptions &options, ImportedScene *out) {
  // RenderCamera carries no transform of its own - Tydra keeps it on the node -
  // so pair each camera with the node that addresses it.
  base::Vector<Mat4> node_matrices;
  node_matrices.resize(static_cast<u32>(scene.cameras.size()));
  base::Vector<bool> found;
  found.resize(static_cast<u32>(scene.cameras.size()));
  for (u32 i = 0; i < found.size(); ++i) found[i] = false;

  struct Finder {
    const tt::RenderScene &scene;
    base::Vector<Mat4> &matrices;
    base::Vector<bool> &found;
    void Visit(const tt::Node &node) {
      if (node.nodeType == tt::NodeType::Camera && node.id >= 0 &&
          static_cast<u32>(node.id) < matrices.size()) {
        matrices[static_cast<u32>(node.id)] = ToMat4(node.global_matrix);
        found[static_cast<u32>(node.id)] = true;
      }
      for (const tt::Node &child : node.children) Visit(child);
    }
  } finder{scene, node_matrices, found};
  for (const tt::Node &node : scene.nodes) finder.Visit(node);

  for (u32 i = 0; i < scene.cameras.size(); ++i) {
    const tt::RenderCamera &src = scene.cameras[i];
    // A hidden camera must not become the viewpoint the scene opens on.
    if (IsHidden(src.abs_path, hidden_paths, options))
      continue;
    ImportedScene::Camera camera;
    // USD cameras look down -Z with +Y up, which is the engine's convention too.
    const Mat4 world = stage_to_engine * (found[i] ? node_matrices[i] : Mat4{});
    camera.position = {world.m[12], world.m[13], world.m[14]};
    const Quat rotation = QuatFromMat4(world);
    camera.rotation[0] = rotation.x;
    camera.rotation[1] = rotation.y;
    camera.rotation[2] = rotation.z;
    camera.rotation[3] = rotation.w;
    if (src.focalLength > 1e-6f) {
      camera.yfov = 2.0f * std::atan(0.5f * src.verticalAperture / src.focalLength);
    }
    camera.znear = src.znear;
    camera.zfar = src.zfar;
    out->cameras.push_back(camera);
  }
}

// Omniverse records its renderer's configuration as an `rtx:` dictionary under
// the layer's customLayerData. None of it is standard USD, so it is read
// defensively: anything missing or of an unexpected type simply leaves the
// engine default in place.
// `fog_start` comes out in stage units; LoadUsdScene converts it once
// metersPerUnit is known.
void ParseRenderSettings(const tinyusdz::Layer &layer,
                         ImportedScene::RenderSettings *out) {
  const auto &custom = layer.metas().customLayerData;
  const auto it = custom.find("renderSettings");
  if (it == custom.end())
    return;
  std::map<std::string, tinyusdz::MetaVariable> settings;
  if (!it->second.get_value(&settings))
    return;

  const auto get_f32 = [&](const char *key, f32 *dst) {
    const auto e = settings.find(key);
    if (e == settings.end())
      return false;
    // Omniverse writes these as float, but a hand-edited stage may carry a
    // double or an int for the same key.
    float f;
    double d;
    int i;
    if (e->second.get_value(&f)) *dst = f;
    else if (e->second.get_value(&d)) *dst = static_cast<f32>(d);
    else if (e->second.get_value(&i)) *dst = static_cast<f32>(i);
    else return false;
    return true;
  };
  const auto get_bool = [&](const char *key, bool *dst) {
    const auto e = settings.find(key);
    if (e == settings.end())
      return false;
    return e->second.get_value(dst);
  };
  const auto get_rgb = [&](const char *key, f32 *dst) {
    const auto e = settings.find(key);
    if (e == settings.end())
      return false;
    tinyusdz::value::float3 v;
    if (!e->second.get_value(&v))
      return false;
    dst[0] = v[0];
    dst[1] = v[1];
    dst[2] = v[2];
    return true;
  };

  out->has_indirect_scale =
      get_f32("rtx:indirectDiffuse:scalingFactor", &out->indirect_scale);
  get_bool("rtx:indirectDiffuse:enabled", &out->indirect_enabled);

  const bool ambient_color =
      get_rgb("rtx:sceneDb:ambientLightColor", out->ambient_color);
  const bool ambient_intensity =
      get_f32("rtx:sceneDb:ambientLightIntensity", &out->ambient_intensity);
  out->has_ambient = ambient_color || ambient_intensity;

  const bool fog_on = get_bool("rtx:fog:enabled", &out->fog_enabled);
  const bool fog_density = get_f32("rtx:fog:fogDistanceDensity", &out->fog_density);
  get_rgb("rtx:fog:fogColor", out->fog_color);
  get_f32("rtx:fog:fogColorIntensity", &out->fog_color_intensity);
  get_f32("rtx:fog:fogStartDist", &out->fog_start);  // stage units for now
  out->has_fog = fog_on || fog_density;

  out->has_lens_flare =
      get_bool("rtx:post:lensFlares:enabled", &out->lens_flare_enabled);
  get_f32("rtx:post:lensFlares:flareScale", &out->lens_flare_scale);
}

// Opening a layer only parses it; nothing composes until asked. This walks
// LIVRPS in strength order and reconstructs a stage from the result. Each arc
// is driven separately rather than through CompositeAllArcs, which hardcodes
// default options and so rejects the parent-relative asset paths below.
bool ComposeStage(const std::string &path, const std::string &base_dir,
                  tinyusdz::Stage *stage,
                  base::Vector<std::string> *hidden_paths,
                  ImportedScene::RenderSettings *render_settings) {
  std::string warn, err;
  tinyusdz::Layer layer;
  if (!tinyusdz::LoadLayerFromFile(path, &layer, &warn, &err)) {
    RX_ERROR("usd {}: {}", path, err.empty() ? "failed to open layer" : err);
    return false;
  }

  tinyusdz::AssetResolutionResolver resolver;
  resolver.set_current_working_path(base_dir);
  resolver.set_search_paths({base_dir});

  // Authored scenes routinely point at `@../Props/thing.usd@`. tinyusdz refuses
  // those by default because a '..' is a sandbox escape for a resolver serving
  // remote or packaged assets; rx reads from the local filesystem the stage
  // already came from, where it is an ordinary relative path.
  tinyusdz::SublayersCompositionOptions sublayer_options;
  sublayer_options.allow_parent_relative_paths = true;
  tinyusdz::ReferencesCompositionOptions reference_options;
  reference_options.allow_parent_relative_paths = true;
  tinyusdz::PayloadCompositionOptions payload_options;
  payload_options.allow_parent_relative_paths = true;
  // Parse each referenced file once and share it across every arc that names
  // it. Production scenes reference the same prop layer hundreds of times.
  std::map<std::string, tinyusdz::Layer> layer_cache;
  reference_options.layer_cache = &layer_cache;
  payload_options.layer_cache = &layer_cache;

  // Sublayers are few and cheap; only tinyusdz's reference/payload passes have
  // an InPlace variant implemented, and those are the ones that blow up.
  {
    tinyusdz::Layer composited;
    if (!tinyusdz::CompositeSublayers(resolver, layer, &composited, &warn, &err,
                                      sublayer_options)) {
      RX_ERROR("usd {}: subLayer composition failed: {}", path, err);
      return false;
    }
    layer = std::move(composited);
  }

  bool settled = false;
  for (int pass = 0; pass < kMaxCompositionPasses && !settled; ++pass) {
    bool pending = false;
    tinyusdz::Layer composited;

    // InPlace: consume the input layer while building the output instead of
    // holding both. On a scene the size of NVIDIA's Attic, where the same prop
    // layers are referenced hundreds of times, that halves the peak.
    if (layer.check_unresolved_references()) {
      pending = true;
      if (!tinyusdz::CompositeReferencesInPlace(
              resolver, std::make_unique<tinyusdz::Layer>(std::move(layer)),
              &composited, &warn, &err, reference_options)) {
        RX_ERROR("usd {}: `references` composition failed: {}", path, err);
        return false;
      }
      layer = std::move(composited);
    }

    if (layer.check_unresolved_payload()) {
      pending = true;
      if (!tinyusdz::CompositePayloadInPlace(
              resolver, std::make_unique<tinyusdz::Layer>(std::move(layer)),
              &composited, &warn, &err, payload_options)) {
        RX_ERROR("usd {}: `payload` composition failed: {}", path, err);
        return false;
      }
      layer = std::move(composited);
    }

    if (layer.check_unresolved_inherits()) {
      pending = true;
      if (!tinyusdz::CompositeInherits(layer, &composited, &warn, &err)) {
        RX_ERROR("usd {}: `inherits` composition failed: {}", path, err);
        return false;
      }
      layer = std::move(composited);
    }

    if (layer.check_unresolved_variant()) {
      pending = true;
      // AOUSD 10.3.2.5: the variantSet a selection names may live in a layer a
      // reference or payload has not pulled in yet, so selection waits for
      // those to resolve rather than binding to an empty placeholder.
      if (!tinyusdz::ShouldDeferVariantComposition(layer)) {
        if (!tinyusdz::CompositeVariant(layer, &composited, &warn, &err)) {
          RX_ERROR("usd {}: `variantSet` composition failed: {}", path, err);
          return false;
        }
        layer = std::move(composited);
      }
    }

    if (layer.check_unresolved_specializes()) {
      pending = true;
      if (!tinyusdz::CompositeSpecializes(layer, &composited, &warn, &err)) {
        RX_ERROR("usd {}: `specializes` composition failed: {}", path, err);
        return false;
      }
      layer = std::move(composited);
    }

    settled = !pending;
  }
  if (!settled) {
    RX_WARN("usd {}: composition did not settle in {} passes; some arcs are "
            "left unresolved",
            path, kMaxCompositionPasses);
  }
  // The layer is fully composed now, so bake the apiSchemas list-ops to
  // explicit form the way `usdcat --flatten` does.
  tinyusdz::FlattenAppliedSchemas(layer);

  u32 recased = 0;
  for (auto &[name, spec] : layer.primspecs())
    recased += NormalizeColorSpaceTokens(spec);
  if (recased > 0)
    RX_DEBUG("usd {}: normalized {} colorSpace token(s)", path, recased);

  u32 relux = 0;
  for (auto &[name, spec] : layer.primspecs())
    relux += NormalizeLuxSchema(spec, "", false, hidden_paths);
  if (relux > 0)
    RX_DEBUG("usd {}: moved {} pre-21.02 light attribute(s) into `inputs:`",
             path, relux);

  if (!warn.empty())
    RX_WARN("usd {}: {}", path, warn);

  // Read before LayerToStage: the layer is moved from there.
  ParseRenderSettings(layer, render_settings);

  if (!tinyusdz::LayerToStage(std::move(layer), stage, &warn, &err)) {
    RX_ERROR("usd {}: could not build a stage from the composed layer: {}",
             path, err);
    return false;
  }
  return true;
}

} // namespace

bool LoadUsdScene(const std::string &path, ImportedScene *out,
                  const UsdLoadOptions &options) {
  std::string warn, err;
  tinyusdz::Stage stage;
  base::Vector<std::string> hidden_paths;
  const std::string base_dir = tinyusdz::io::GetBaseDir(path);
  const bool is_usdz = tinyusdz::IsUSDZ(path);
  // usdz goes through the same path as a loose stage: a package selects its own
  // root layer (first .usdc, else first .usda), and tinyusdz's layer reader
  // picks it the same way its stage reader does. Reading it as a layer is what
  // gives a package the legacy-UsdLux normalization and the visibility pass
  // below - loading it straight to a Stage skips both, so packaged legacy
  // lights reconstruct as defaults and authored-invisible prims stay lit.
  if (!ComposeStage(path, base_dir, &stage, &hidden_paths,
                    &out->render_settings)) {
    return false;
  }

  tt::RenderSceneConverterEnv env(stage);
  env.usd_filename = path;
  env.mesh_config.triangulate = true;
  // Make every vertex attribute share one index buffer, which is what the
  // engine's vertex format needs; without it uvs and normals stay facevarying.
  env.mesh_config.build_vertex_indices = true;
  env.mesh_config.compute_tangents_and_binormals = true;
  env.scene_config.load_texture_assets = true;
  // EXPERIMENT (not part of the PR): without this Tydra widens every 8-bit
  // sRGB texture to fp32, which ConvertImage's UInt8-only gate then rejects.
  env.material_config.preserve_texel_bitdepth = true;
  if (!is_usdz) {
    // Inside a usdz package the stock loader reads through the package
    // resolver, which is the only thing that can see those entries.
    env.material_config.texture_image_loader_function = LoadTextureAsset;
    env.material_config.texture_image_loader_function_userdata =
        const_cast<std::string *>(&base_dir);
  }

  tinyusdz::USDZAsset usdz_asset;
  if (is_usdz) {
    // Textures live inside the zip; resolve them through it.
    if (!tinyusdz::ReadUSDZAssetInfoFromFile(path, &usdz_asset, &warn, &err)) {
      RX_ERROR("usd {}: unreadable usdz package: {}", path, err);
      return false;
    }
    if (!tinyusdz::SetupUSDZAssetResolution(env.asset_resolver, &usdz_asset)) {
      RX_ERROR("usd {}: could not resolve assets inside the usdz package", path);
      return false;
    }
  } else {
    env.set_search_paths({base_dir});
  }

  tt::RenderScene render_scene;
  tt::RenderSceneConverter converter;
  if (!converter.ConvertToRenderScene(env, &render_scene)) {
    RX_ERROR("usd {}: {}", path, converter.GetError());
    return false;
  }
  if (const std::string convert_warn = converter.GetWarning(); !convert_warn.empty())
    RX_WARN("usd {}: {}", path, convert_warn);

  base::Vector<AssetId> image_ids;
  image_ids.resize(static_cast<u32>(render_scene.images.size()));
  u32 undecoded = 0;
  for (size_t i = 0; i < render_scene.images.size(); ++i) {
    Texture texture;
    texture.id = ScopedId(path, "tex", i);
    if (!ConvertImage(render_scene, render_scene.images[i], &texture)) {
      ++undecoded;
      continue;
    }
    image_ids[static_cast<u32>(i)] = texture.id;
    out->textures.push_back(std::move(texture));
  }

  base::Vector<AssetId> material_ids;
  material_ids.resize(static_cast<u32>(render_scene.materials.size()));
  u32 unsupported_shaders = 0;
  u32 openpbr_shaders = 0;
  for (size_t i = 0; i < render_scene.materials.size(); ++i) {
    Material material;
    material.id = ScopedId(path, "mat", i);
    if (render_scene.materials[i].openPBRShader)
      ++openpbr_shaders;
    else if (!render_scene.materials[i].surfaceShader)
      ++unsupported_shaders;
    ConvertMaterial(render_scene, render_scene.materials[i], image_ids,
                    &material);
    material_ids[static_cast<u32>(i)] = material.id;
    out->materials.push_back(std::move(material));
  }

  // Mesh indices have to stay aligned with RenderScene::meshes because nodes
  // address meshes by index, so a mesh that fails conversion keeps its slot.
  u32 skipped_meshes = 0;
  for (size_t i = 0; i < render_scene.meshes.size(); ++i) {
    Mesh mesh;
    mesh.id = ScopedId(path, "mesh", i);
    if (!ConvertMesh(render_scene.meshes[i], material_ids, &mesh)) {
      ++skipped_meshes;
      RX_DEBUG("usd {}: skipped mesh {}", path,
               render_scene.meshes[i].abs_path);
    }
    out->meshes.push_back(std::move(mesh));
  }

  // Normalize the stage's own conventions away: rotate a z-up stage into the
  // engine's y-up, then fold metersPerUnit in so world units are metres.
  const f32 meters_per_unit =
      static_cast<f32>(render_scene.meta.metersPerUnit);
  Mat4 stage_to_engine = MakeScale(meters_per_unit > 0 ? meters_per_unit : 1.0f);
  if (render_scene.meta.upAxis == "Z") {
    stage_to_engine =
        stage_to_engine *
        MakeFromQuat(QuatFromAxisAngle({1, 0, 0}, -3.14159265358979f * 0.5f));
  }

  out->render_settings.fog_start *= meters_per_unit;

  ConvertLights(render_scene, stage_to_engine, meters_per_unit, hidden_paths,
                options, base_dir, out);
  ConvertCameras(render_scene, stage_to_engine, hidden_paths, options, out);

  NodeWalk walk{render_scene, stage_to_engine, out};
  for (const tt::Node &node : render_scene.nodes)
    walk.Visit(node);
  // A stage whose nodes carry no geometry (some exporters emit a flat mesh
  // list) still has meshes worth showing, at the origin.
  if (out->instances.empty()) {
    for (u32 i = 0; i < out->meshes.size(); ++i) {
      if (!out->meshes[i].lods.empty())
        out->instances.push_back(MakeInstance(i, stage_to_engine));
    }
  }

  RX_INFO("usd {}: {} meshes, {} materials, {} textures, {} instances, "
          "{} lights, {} cameras (upAxis {}, {} m/unit)",
          path, out->meshes.size() - skipped_meshes, out->materials.size(),
          out->textures.size(), out->instances.size(), out->lights.size(),
          out->cameras.size(), render_scene.meta.upAxis, meters_per_unit);
  if (openpbr_shaders)
    RX_INFO("usd {}: {} OpenPBR material(s)", path, openpbr_shaders);
  if (skipped_meshes || undecoded || unsupported_shaders || walk.mirrored) {
    RX_WARN("usd {}: {} mesh(es) not representable, {} image(s) not decoded, "
            "{} material(s) with neither a UsdPreviewSurface nor an OpenPBR "
            "surface, {} mirrored instance(s)",
            path, skipped_meshes, undecoded, unsupported_shaders,
            walk.mirrored);
  }
  return true;
}

#endif // RX_HAVE_USD

} // namespace rx::asset
