#include "asset/usd_loader.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
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

bool LoadUsdScene(const std::string &path, ImportedScene *) {
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

void ConvertMaterial(const tt::RenderScene &scene, const tt::RenderMaterial &src,
                     const base::Vector<AssetId> &image_ids, Material *out) {
  out->name = src.name;
  if (!src.surfaceShader)
    return; // MDL- or MaterialX-only material: keep the engine defaults.

  const tt::PreviewSurfaceShader &s = *src.surfaceShader;
  const auto texture = [&](i32 texture_id) -> AssetId {
    const i64 image = ImageIndexOf(scene, texture_id);
    return image >= 0 ? image_ids[static_cast<u32>(image)] : AssetId{};
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

  // UsdPreviewSurface has no packed ORM channel: roughness and metallic are
  // separate inputs. When both resolve to one image the author packed them
  // glTF-style and the combined path applies; otherwise the roughness map goes
  // in the combined slot (read through .g, which the greyscale expansion above
  // keeps valid) and metallic gets its own.
  const i64 roughness_image = ImageIndexOf(scene, s.roughness.texture_id);
  const i64 metallic_image = ImageIndexOf(scene, s.metallic.texture_id);
  if (roughness_image >= 0)
    out->metallic_roughness = image_ids[static_cast<u32>(roughness_image)];
  if (metallic_image >= 0 && metallic_image != roughness_image) {
    out->metallic_map = image_ids[static_cast<u32>(metallic_image)];
    out->separate_metallic = true;
    if (roughness_image < 0)
      out->metallic_roughness = out->metallic_map;
  } else if (metallic_image >= 0 && roughness_image < 0) {
    out->metallic_roughness = image_ids[static_cast<u32>(metallic_image)];
  }

  switch (src.materialTag) {
  case tt::MaterialTag::Masked:
    out->alpha_mode = AlphaMode::kMask;
    out->alpha_cutoff = s.opacityThreshold.value;
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

// Opening a layer only parses it; nothing composes until asked. This walks
// LIVRPS in strength order and reconstructs a stage from the result. Each arc
// is driven separately rather than through CompositeAllArcs, which hardcodes
// default options and so rejects the parent-relative asset paths below.
bool ComposeStage(const std::string &path, const std::string &base_dir,
                  tinyusdz::Stage *stage) {
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

  if (!warn.empty())
    RX_WARN("usd {}: {}", path, warn);

  if (!tinyusdz::LayerToStage(std::move(layer), stage, &warn, &err)) {
    RX_ERROR("usd {}: could not build a stage from the composed layer: {}",
             path, err);
    return false;
  }
  return true;
}

} // namespace

bool LoadUsdScene(const std::string &path, ImportedScene *out) {
  std::string warn, err;
  tinyusdz::Stage stage;
  const std::string base_dir = tinyusdz::io::GetBaseDir(path);
  const bool is_usdz = tinyusdz::IsUSDZ(path);
  if (is_usdz) {
    // A usdz package selects its own root layer (first .usdc, else first
    // .usda), which only tinyusdz's package reader knows how to pick. Packages
    // are self-contained by spec, so they arrive effectively flattened.
    if (!tinyusdz::LoadUSDFromFile(path, &stage, &warn, &err)) {
      RX_ERROR("usd {}: {}", path, err.empty() ? "failed to open stage" : err);
      return false;
    }
    if (!warn.empty())
      RX_WARN("usd {}: {}", path, warn);
  } else if (!ComposeStage(path, base_dir, &stage)) {
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
  for (size_t i = 0; i < render_scene.materials.size(); ++i) {
    Material material;
    material.id = ScopedId(path, "mat", i);
    if (!render_scene.materials[i].surfaceShader)
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

  RX_INFO("usd {}: {} meshes, {} materials, {} textures, {} instances "
          "(upAxis {}, {} m/unit)",
          path, out->meshes.size() - skipped_meshes, out->materials.size(),
          out->textures.size(), out->instances.size(),
          render_scene.meta.upAxis, meters_per_unit);
  if (skipped_meshes || undecoded || unsupported_shaders || walk.mirrored) {
    RX_WARN("usd {}: {} mesh(es) not representable, {} image(s) not decoded, "
            "{} material(s) without a UsdPreviewSurface, {} mirrored instance(s)",
            path, skipped_meshes, undecoded, unsupported_shaders,
            walk.mirrored);
  }
  return true;
}

#endif // RX_HAVE_USD

} // namespace rx::asset
