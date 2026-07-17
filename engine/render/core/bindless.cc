#include "render/core/bindless.h"

#include <algorithm>
#include <cstring>

#include "core/log.h"

namespace rx::render {

std::unique_ptr<BindlessRegistry> BindlessRegistry::Create(Device& device) {
  auto registry = std::unique_ptr<BindlessRegistry>(new BindlessRegistry(device));
  if (!registry->Initialize()) return nullptr;
  return registry;
}

bool BindlessRegistry::Initialize() {
  mesh_table_ = device_.CreateBuffer(sizeof(MeshRecord) * kMaxMeshes, kBufferUsageStorage, true);
  geometry_table_ =
      device_.CreateBuffer(sizeof(GeometryRecord) * kMaxGeometries, kBufferUsageStorage, true);
  material_table_ =
      device_.CreateBuffer(sizeof(MaterialRecord) * kMaxMaterials, kBufferUsageStorage, true);
  if (!mesh_table_.mapped || !geometry_table_.mapped || !material_table_.mapped) return false;

  // Hit shading runs from ddgi compute and the water fragment shader.
  set_layout_ = device_.CreateBindingLayout({
      .stages = kShaderStageCompute | kShaderStageFragment,
      .slots = {{0, BindingType::kStorageBuffer},
                {1, BindingType::kStorageBuffer},
                {2, BindingType::kStorageBuffer},
                {3, BindingType::kSampledImage, kMaxTextures, /*variable_count=*/true},
                {4, BindingType::kSampler},
                // Per-mesh vertex/index buffers for the DXIL geometry readers
                // (the SPIR-V path reads the same buffers through BDA and
                // never declares this binding).
                {kGeometryBufferBinding, BindingType::kByteBuffer, kMaxGeometryBuffers,
                 /*variable_count=*/true}},
      .update_after_bind = true,
  });
  if (!set_layout_) return false;

  set_ = device_.CreateBindingSet(set_layout_, kMaxTextures);
  if (!set_) return false;

  SamplerHandle sampler = device_.GetSampler({});  // trilinear repeat
  device_.UpdateBindingSet(set_, {Bind::StorageBuffer(0, mesh_table_),
                                  Bind::StorageBuffer(1, geometry_table_),
                                  Bind::StorageBuffer(2, material_table_),
                                  Bind::Sampler(4, sampler)});
  return true;
}

u32 BindlessRegistry::RegisterTexture(TextureView view) {
  u32 index;
  if (!free_textures_.empty()) {
    index = free_textures_.back();
    free_textures_.pop_back();
  } else if (texture_count_ < kMaxTextures) {
    index = texture_count_++;
  } else {
    RX_WARN("bindless texture table full");
    return kInvalidIndex;
  }
  BindingItem item = Bind::SampledView(3, view);
  item.array_index = index;
  device_.UpdateBindingSet(set_, {item});
  return index;
}

void BindlessRegistry::ReleaseTexture(u32 index) {
  if (index == kInvalidIndex) return;
  free_textures_.push_back(index);
}

void BindlessRegistry::RewriteTextureIndex(u32 material_index, u32 old_texture, u32 new_texture) {
  if (material_index == kInvalidIndex || material_index >= material_count_) return;
  auto* record = reinterpret_cast<MaterialRecord*>(static_cast<u8*>(material_table_.mapped) +
                                                   material_index * sizeof(MaterialRecord));
  if (record->base_color_texture == old_texture) record->base_color_texture = new_texture;
  if (record->metallic_roughness_texture == old_texture) {
    record->metallic_roughness_texture = new_texture;
  }
  if (record->terrain_layer1_texture == old_texture) record->terrain_layer1_texture = new_texture;
  if (record->terrain_weight_texture == old_texture) record->terrain_weight_texture = new_texture;
}

u32 BindlessRegistry::RegisterMaterial(const MaterialRecord& record) {
  if (material_count_ >= kMaxMaterials) {
    RX_WARN("bindless material table full");
    return kInvalidIndex;
  }
  u32 index = material_count_++;
  std::memcpy(static_cast<u8*>(material_table_.mapped) + index * sizeof(MaterialRecord), &record,
              sizeof(record));
  return index;
}

u32 BindlessRegistry::RegisterMesh(const GpuBuffer& vertices, const GpuBuffer& indices,
                                   const GeometryRecord* geometries, u32 geometry_count) {
  if ((free_meshes_.empty() && mesh_count_ >= kMaxMeshes) ||
      geometry_count > kMaxGeometries) {
    RX_WARN("bindless mesh tables full");
    return kInvalidIndex;
  }
  u32 geometry_offset = geometry_count_;
  size_t free_range = free_geometry_ranges_.size();
  for (size_t i = 0; i < free_geometry_ranges_.size(); ++i) {
    if (free_geometry_ranges_[i].count >= geometry_count) {
      geometry_offset = free_geometry_ranges_[i].offset;
      free_range = i;
      break;
    }
  }
  if (free_range == free_geometry_ranges_.size() &&
      geometry_count_ + geometry_count > kMaxGeometries) {
    RX_WARN("bindless geometry table full");
    return kInvalidIndex;
  }
  if (free_range != free_geometry_ranges_.size()) {
    GeometryRange& range = free_geometry_ranges_[free_range];
    range.offset += geometry_count;
    range.count -= geometry_count;
    if (range.count == 0) free_geometry_ranges_.erase(free_range);
  } else {
    geometry_count_ += geometry_count;
  }
  u32 index = 0;
  if (!free_meshes_.empty()) {
    index = free_meshes_.back();
    free_meshes_.pop_back();
  } else {
    index = mesh_count_++;
    active_meshes_.resize(mesh_count_);
    mesh_geometry_counts_.resize(mesh_count_);
  }
  MeshRecord record;
  record.vertex_address = vertices.address;
  record.index_address = indices.address;
  record.geometry_offset = geometry_offset;
  // Mirror the buffers into the geometry buffer array for the DXIL readers.
  record.vertex_srv = 2 * index;
  record.index_srv = 2 * index + 1;
  BindingItem vertex_srv = Bind::ByteBuffer(kGeometryBufferBinding, vertices);
  vertex_srv.array_index = record.vertex_srv;
  BindingItem index_srv = Bind::ByteBuffer(kGeometryBufferBinding, indices);
  index_srv.array_index = record.index_srv;
  device_.UpdateBindingSet(set_, {vertex_srv, index_srv});
  if (geometry_count != 0) {
    std::memcpy(static_cast<u8*>(geometry_table_.mapped) +
                    geometry_offset * sizeof(GeometryRecord),
                geometries, geometry_count * sizeof(GeometryRecord));
  }
  std::memcpy(static_cast<u8*>(mesh_table_.mapped) + index * sizeof(MeshRecord), &record,
              sizeof(record));
  active_meshes_[index] = 1;
  mesh_geometry_counts_[index] = geometry_count;
  return index;
}

void BindlessRegistry::ReleaseMesh(u32 index) {
  if (index >= mesh_count_ || !active_meshes_[index]) return;
  auto* record = reinterpret_cast<MeshRecord*>(
      static_cast<u8*>(mesh_table_.mapped) + index * sizeof(MeshRecord));
  const u32 geometry_count = mesh_geometry_counts_[index];
  if (geometry_count != 0) {
    free_geometry_ranges_.push_back({record->geometry_offset, geometry_count});
    std::sort(free_geometry_ranges_.begin(), free_geometry_ranges_.end(),
              [](const GeometryRange& a, const GeometryRange& b) {
                return a.offset < b.offset;
              });
    for (size_t i = 1; i < free_geometry_ranges_.size();) {
      GeometryRange& previous = free_geometry_ranges_[i - 1];
      const GeometryRange current = free_geometry_ranges_[i];
      if (previous.offset + previous.count == current.offset) {
        previous.count += current.count;
        free_geometry_ranges_.erase(i);
      } else {
        ++i;
      }
    }
  }
  *record = {};
  active_meshes_[index] = 0;
  mesh_geometry_counts_[index] = 0;
  free_meshes_.push_back(index);
}

BindlessRegistry::~BindlessRegistry() {
  device_.DestroyBuffer(mesh_table_);
  device_.DestroyBuffer(geometry_table_);
  device_.DestroyBuffer(material_table_);
  device_.DestroyBindingSet(set_);
  device_.DestroyBindingLayout(set_layout_);
}

}  // namespace rx::render
