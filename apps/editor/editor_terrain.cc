#include "editor_app.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <limits>
#include <string>
#include <utility>

#include "asset/gltf_loader.h"
#include "core/log.h"
#include "edit/reflect.h"
#include "render/core/renderer.h"
#include "scene/components.h"

namespace rx::editor {
namespace fs = std::filesystem;

namespace {

const char* BrushLabel(terrain::TerrainBrushMode mode) {
  switch (mode) {
    case terrain::TerrainBrushMode::kRaise: return "Raise Terrain";
    case terrain::TerrainBrushMode::kLower: return "Lower Terrain";
    case terrain::TerrainBrushMode::kSmooth: return "Smooth Terrain";
    case terrain::TerrainBrushMode::kFlatten: return "Flatten Terrain";
    case terrain::TerrainBrushMode::kPaintLayer: return "Paint Terrain";
  }
  return "Edit Terrain";
}

u32 PackRgba(const std::array<u8, 4>& color) {
  return (static_cast<u32>(color[0]) << 24) | (static_cast<u32>(color[1]) << 16) |
         (static_cast<u32>(color[2]) << 8) | color[3];
}

class TerrainCommand final : public edit::Command {
 public:
  using Refresh = std::function<void(std::span<const terrain::TerrainTileKey>)>;

  TerrainCommand(terrain::Terrain* target, terrain::TerrainChange change, std::string label,
                 Refresh refresh)
      : target_(target), change_(std::move(change)), label_(std::move(label)),
        refresh_(std::move(refresh)) {}

  void Apply(ecs::World&) override {
    if (target_->ApplyChange(change_)) RefreshTiles();
  }

  void Revert(ecs::World&) override {
    if (target_->RevertChange(change_)) RefreshTiles();
  }

  const char* label() const override { return label_.c_str(); }

 private:
  void RefreshTiles() {
    refresh_(std::span<const terrain::TerrainTileKey>(change_.dirty_tiles.data(),
                                                      change_.dirty_tiles.size()));
  }

  terrain::Terrain* target_;
  terrain::TerrainChange change_;
  std::string label_;
  Refresh refresh_;
};

}  // namespace

void Editor::SetupDefaultTerrain() {
  terrain::TerrainDesc desc;
  desc.id = asset::MakeAssetId("editor/default_terrain");
  desc.origin = {0, 0, 0};
  desc.tile_quads = 16;
  desc.sample_spacing = 0.5f;

  auto add_layer = [&](const char* name, std::array<u8, 4> color) {
    terrain::TerrainLayer layer;
    layer.name = name;
    layer.debug_rgba = color;
    desc.layers.push_back(std::move(layer));
  };
  add_layer("Meadow", {82, 122, 67, 255});
  add_layer("Earth", {126, 87, 55, 255});
  add_layer("Stone", {118, 124, 128, 255});
  add_layer("Sand", {184, 157, 103, 255});

  ClearTerrainVisuals();
  terrain_ = terrain::Terrain(std::move(desc));
  const u32 side = terrain_.samples_per_side();
  const u32 quads = terrain_.desc().tile_quads;
  const f32 spacing = terrain_.desc().sample_spacing;
  std::vector<f32> heights(static_cast<size_t>(side) * side);
  std::vector<terrain::TerrainWeights> weights(heights.size());

  for (i32 tile_z = -2; tile_z < 2; ++tile_z) {
    for (i32 tile_x = -2; tile_x < 2; ++tile_x) {
      for (u32 z = 0; z < side; ++z) {
        for (u32 x = 0; x < side; ++x) {
          const i32 grid_x = tile_x * static_cast<i32>(quads) + static_cast<i32>(x);
          const i32 grid_z = tile_z * static_cast<i32>(quads) + static_cast<i32>(z);
          const f32 world_x = grid_x * spacing;
          const f32 world_z = grid_z * spacing;
          const f32 broad = 0.46f * std::sin(world_x * 0.22f) * std::cos(world_z * 0.18f);
          const f32 detail = 0.17f * std::sin((world_x + world_z) * 0.48f) +
                             0.10f * std::cos((world_x - world_z) * 0.61f);
          const f32 knoll = 0.30f * std::exp(-(world_x * world_x + world_z * world_z) / 95.0f);
          const size_t sample = static_cast<size_t>(z) * side + x;
          heights[sample] = broad + detail + knoll;

          const f32 patch = std::sin(world_x * 0.31f) * std::cos(world_z * 0.27f);
          if (heights[sample] > 0.62f) {
            weights[sample].rgba = {40, 25, 190, 0};
          } else if (heights[sample] < -0.25f) {
            weights[sample].rgba = {35, 20, 0, 200};
          } else if (patch > 0.58f) {
            weights[sample].rgba = {90, 165, 0, 0};
          } else {
            weights[sample].rgba = {220, 35, 0, 0};
          }
        }
      }
      if (!terrain_.AddOrReplaceTile({tile_x, tile_z}, heights, weights))
        RX_WARN("editor: failed to create terrain tile {},{}", tile_x, tile_z);
    }
  }

  terrain_path_ = "untitled.rxterrain";
  terrain_dirty_ = false;
  RebuildTerrainVisuals();
}

ecs::Entity Editor::SpawnTerrainTile(terrain::TerrainTileKey key, asset::AssetId mesh) {
  const f32 width = terrain_.desc().tile_quads * terrain_.desc().sample_spacing;
  const Vec3& origin = terrain_.desc().origin;
  ecs::Entity entity = world_->Create();
  world_->Add(entity, scene::Transform{.position = {
                                          origin.x + key.x * width,
                                          origin.y,
                                          origin.z + key.z * width,
                                      }});
  world_->Add(entity, scene::Renderable{mesh});
  return entity;
}

void Editor::ClearTerrainVisuals() {
  if (world_) {
    for (const auto& [key, visual] : terrain_tiles_) {
      (void)key;
      if (world_->IsAlive(visual.entity)) world_->Destroy(visual.entity);
      if (renderer_) renderer_->RemoveDynamicMesh(visual.mesh);
      meshes_.erase(visual.mesh.hash);
    }
  }
  terrain_tiles_.clear();
  terrain_cursor_hit_.reset();
  placement_preview_.reset();
}

void Editor::RebuildTerrainVisuals() {
  ClearTerrainVisuals();
  for (const terrain::TerrainTile& tile : terrain_.tiles()) {
    std::optional<asset::Mesh> mesh = terrain_.BuildTileMesh(tile.key, terrain_material_);
    if (!mesh) {
      RX_WARN("editor: failed to build terrain tile {},{}", tile.key.x, tile.key.z);
      continue;
    }
    if (renderer_ && !renderer_->UpdateDynamicMesh(*mesh) &&
        !renderer_->UploadMesh(*mesh)) {
      RX_WARN("editor: failed to upload terrain tile {},{}", tile.key.x,
              tile.key.z);
    }
    const asset::AssetId mesh_id = mesh->id;
    const std::string name = "Terrain [" + std::to_string(tile.key.x) + "," +
                             std::to_string(tile.key.z) + "]";
    meshes_[mesh_id.hash] = MeshRecord{std::move(*mesh), name};
    ecs::Entity entity = SpawnTerrainTile(tile.key, mesh_id);
    terrain_tiles_.emplace(tile.key, TerrainTileVisual{tile.key, entity, mesh_id});
  }
}

void Editor::RebuildTerrainTiles(std::span<const terrain::TerrainTileKey> keys, bool live) {
  for (terrain::TerrainTileKey key : keys) {
    auto visual = terrain_tiles_.find(key);
    if (visual == terrain_tiles_.end()) {
      RX_WARN("editor: no visual mapping for dirty terrain tile {},{}", key.x, key.z);
      continue;
    }
    std::optional<asset::Mesh> mesh = terrain_.BuildTileMesh(key, terrain_material_);
    if (!mesh) continue;
    if (renderer_) {
      const bool updated = live ? renderer_->UpdateDynamicMesh(*mesh)
                                : renderer_->UploadMesh(*mesh);
      if (!updated)
        RX_WARN("editor: failed to {} terrain tile {},{}",
                live ? "update" : "upload", key.x, key.z);
    }
    const std::string name = "Terrain [" + std::to_string(key.x) + "," +
                             std::to_string(key.z) + "]";
    meshes_[mesh->id.hash] = MeshRecord{std::move(*mesh), name};
  }
}

bool Editor::IsTerrainVisual(ecs::Entity entity) const {
  for (const auto& [key, visual] : terrain_tiles_) {
    (void)key;
    if (visual.entity == entity) return true;
  }
  return false;
}

std::pair<Vec3, Vec3> Editor::ViewportCameraRay(f32 mx, f32 my) const {
  const f32 width = static_cast<f32>(window_->width());
  const f32 height = static_cast<f32>(window_->height());
  const f32 ndc_x = 2.0f * mx / width - 1.0f;
  const f32 ndc_y = 1.0f - 2.0f * my / height;
  const f32 tan_half_fov = std::tan(1.0472f * 0.5f);
  const Vec3 forward = camera_.forward();
  const Vec3 right = Normalize(Cross(forward, {0, 1, 0}));
  const Vec3 up = Cross(right, forward);
  const Vec3 direction = Normalize(forward + right * (ndc_x * width / height * tan_half_fov) +
                                   up * (ndc_y * tan_half_fov));
  return {camera_.position(), direction};
}

void Editor::RecordTerrainChange(terrain::TerrainChange change, const std::string& label) {
  if (change.empty()) return;
  undo_.RecordApplied(std::make_unique<TerrainCommand>(
      &terrain_, std::move(change), label,
      [this](std::span<const terrain::TerrainTileKey> keys) { OnTerrainCommandReplayed(keys); }));
}

void Editor::OnTerrainCommandReplayed(std::span<const terrain::TerrainTileKey> keys) {
  terrain_command_replayed_ = true;
  terrain_dirty_ = true;
  RebuildTerrainTiles(keys, true);
  SyncTerrainRayTracing(keys);
  MarkDirty();
}

void Editor::SyncTerrainRayTracing(
    std::span<const terrain::TerrainTileKey> keys) {
  if (!renderer_) return;
  for (terrain::TerrainTileKey key : keys) {
    const auto visual = terrain_tiles_.find(key);
    if (visual == terrain_tiles_.end()) continue;
    const MeshRecord* record = FindMesh(visual->second.mesh.hash);
    if (record) renderer_->SyncDynamicMeshRayTracing(record->mesh);
  }
}

void Editor::FinishTerrainStroke() {
  if (!terrain_stroke_.active) return;
  if (!terrain_stroke_.change.empty()) {
    SyncTerrainRayTracing(terrain_stroke_.change.dirty_tiles);
    RecordTerrainChange(std::move(terrain_stroke_.change), terrain_stroke_.label);
  }
  terrain_stroke_ = {};
}

void Editor::FinishPlacementDrag() {
  if (!placement_.dragging) return;
  undo_.EndGroup();
  placement_.dragging = false;
}

void Editor::SetEditorMode(EditorMode mode) {
  if (editor_mode_ == EditorMode::kTerrain && mode != EditorMode::kTerrain)
    FinishTerrainStroke();
  if (editor_mode_ == EditorMode::kPlace && mode != EditorMode::kPlace)
    FinishPlacementDrag();
  if (gizmo_drag_.active) {
    undo_.EndGroup();
    gizmo_drag_.active = false;
  }
  if (scrub_.active) {
    undo_.EndGroup();
    scrub_ = {};
  }
  if (mode == EditorMode::kSelect) {
    placement_ = {};
    placement_preview_.reset();
  } else if (mode == EditorMode::kTerrain) {
    placement_ = {};
    placement_preview_.reset();
    selection_.Clear();
  }
  editor_mode_ = mode;
  pick_pending_ = false;
  MarkDirty();
}

void Editor::UpdateModeInteraction(bool lmb_down, bool lmb_edge) {
  const InputState& input = window_->input();
  if (input.button(MouseButton::kRight) || camera_.looking()) {
    FinishTerrainStroke();
    FinishPlacementDrag();
    return;
  }
  const bool over_viewport = CursorOverViewport() && !dialog_open_;

  if (editor_mode_ == EditorMode::kTerrain) {
    terrain_cursor_hit_.reset();
    if (over_viewport) {
      auto [origin, direction] = ViewportCameraRay(input.mouse_x, input.mouse_y);
      terrain_cursor_hit_ = terrain_.Raycast(origin, direction, 1000.0f);
    }
    if (!lmb_down) {
      FinishTerrainStroke();
      return;
    }
    if (lmb_edge && terrain_cursor_hit_) {
      terrain_stroke_ = {};
      terrain_stroke_.active = true;
      terrain_stroke_.last_dab = terrain_cursor_hit_->position;
      terrain_stroke_.flatten_target = terrain_cursor_hit_->position.y;
      terrain_stroke_.label = BrushLabel(terrain_brush_mode_);
    }
    if (!terrain_stroke_.active || !terrain_cursor_hit_ || !over_viewport) return;

    base::Vector<terrain::TerrainTileKey> dirty_tiles;
    auto apply_dab = [&](const Vec3& position) {
      terrain::TerrainBrushMode mode = terrain_brush_mode_;
      if (input.key(Key::kLeftShift)) {
        if (mode == terrain::TerrainBrushMode::kRaise)
          mode = terrain::TerrainBrushMode::kLower;
        else if (mode == terrain::TerrainBrushMode::kLower)
          mode = terrain::TerrainBrushMode::kRaise;
      }
      terrain::TerrainBrush brush;
      brush.mode = mode;
      brush.center_x = position.x;
      brush.center_z = position.z;
      brush.radius = terrain_brush_radius_;
      brush.strength = terrain_brush_strength_;
      brush.falloff = terrain_brush_falloff_;
      brush.flatten_target = terrain_stroke_.flatten_target;
      brush.layer = terrain_brush_layer_;
      terrain::TerrainChange dab = terrain_.ApplyBrush(brush);
      if (dab.empty()) return;
      for (terrain::TerrainTileKey key : dab.dirty_tiles)
        dirty_tiles.push_back(key);
      if (terrain_stroke_.change.empty()) {
        terrain_stroke_.change = std::move(dab);
      } else if (!terrain::MergeTerrainChanges(&terrain_stroke_.change, dab)) {
        RX_WARN("editor: could not merge terrain brush dab into stroke");
      }
      terrain_dirty_ = true;
      MarkDirty();
    };
    auto upload_dirty_tiles = [&] {
      std::sort(dirty_tiles.begin(), dirty_tiles.end());
      dirty_tiles.erase(std::unique(dirty_tiles.begin(), dirty_tiles.end()),
                        dirty_tiles.end());
      RebuildTerrainTiles(dirty_tiles, true);
    };

    const Vec3 current = terrain_cursor_hit_->position;
    if (lmb_edge) {
      apply_dab(current);
      upload_dirty_tiles();
      terrain_stroke_.last_dab = current;
      return;
    }
    const Vec3 delta = current - terrain_stroke_.last_dab;
    const f32 distance = Length(delta);
    const f32 spacing = std::max(terrain_.desc().sample_spacing * 0.5f,
                                 terrain_brush_radius_ * 0.20f);
    if (distance < spacing) return;
    const int steps = std::min(64, static_cast<int>(distance / spacing));
    const Vec3 start = terrain_stroke_.last_dab;
    for (int i = 1; i <= steps; ++i)
      apply_dab(start + delta * (static_cast<f32>(i) * spacing / distance));
    upload_dirty_tiles();
    terrain_stroke_.last_dab = start + delta * (static_cast<f32>(steps) * spacing / distance);
    return;
  }

  terrain_cursor_hit_.reset();
  if (editor_mode_ != EditorMode::kPlace || !placement_.armed) {
    placement_preview_.reset();
    return;
  }

  placement_preview_.reset();
  f32 vertical_offset = 0;
  if (const MeshRecord* record = FindMesh(placement_.mesh.hash);
      record && !record->mesh.lods.empty() && !record->mesh.lods[0].vertices.empty()) {
    f32 min_y = std::numeric_limits<f32>::max();
    for (const asset::Vertex& vertex : record->mesh.lods[0].vertices)
      min_y = std::min(min_y, vertex.position[1]);
    vertical_offset = -min_y;
  }
  if (over_viewport) {
    auto [origin, direction] = ViewportCameraRay(input.mouse_x, input.mouse_y);
    terrain_cursor_hit_ = terrain_.Raycast(origin, direction, 1000.0f);
    Vec3 position;
    if (terrain_cursor_hit_) {
      position = terrain_cursor_hit_->position;
    } else {
      const f32 plane_t = std::fabs(direction.y) > 1e-5f ? -origin.y / direction.y : -1.0f;
      position = plane_t > 0 ? origin + direction * plane_t : origin + direction * 5.0f;
    }
    position.y += vertical_offset;
    placement_preview_ = position;
  }

  if (!lmb_down) {
    FinishPlacementDrag();
    return;
  }
  if (!placement_preview_ || !over_viewport) return;

  auto place = [&](const Vec3& position) {
    const edit::ComponentDesc* transform = edit::FindComponentByName("Transform");
    const edit::ComponentDesc* renderable = edit::FindComponentByName("Renderable");
    const edit::ComponentDesc* name = edit::FindComponentByName("Name");
    if (!transform || !renderable || !name) return;
    std::vector<std::pair<const edit::ComponentDesc*,
                          std::vector<std::pair<const edit::PropDesc*, edit::PropValue>>>>
        initial;
    initial.push_back(
        {transform, {{&transform->props[0], edit::PropValue::Vec3(position.x, position.y,
                                                                  position.z)}}});
    initial.push_back(
        {renderable, {{&renderable->props[0], edit::PropValue::AssetIdV(placement_.mesh.hash)}}});
    initial.push_back({name, {{&name->props[0], edit::PropValue::String(placement_.name)}}});
    undo_.Push(*world_, edit::MakeCreateEntity(std::move(initial), nullptr));
    doc_dirty_ = true;
    MarkDirty();
  };

  if (lmb_edge) {
    const std::string label = "Place " + placement_.name;
    undo_.BeginGroup(label.c_str());
    placement_.dragging = true;
    placement_.last_position = *placement_preview_;
    place(*placement_preview_);
    return;
  }
  if (!placement_.dragging) return;
  const Vec3 delta = *placement_preview_ - placement_.last_position;
  const f32 distance = Length(delta);
  if (distance < placement_.spacing) return;
  const Vec3 start = placement_.last_position;
  const int steps = std::min(64, static_cast<int>(distance / placement_.spacing));
  for (int i = 1; i <= steps; ++i) {
    Vec3 position = start + delta * (static_cast<f32>(i) * placement_.spacing / distance);
    if (std::optional<f32> height = terrain_.SampleHeight(position.x, position.z))
      position.y = *height + vertical_offset;
    place(position);
  }
  placement_.last_position =
      start + delta * (static_cast<f32>(steps) * placement_.spacing / distance);
}

void Editor::AppendInteractionPreview(std::vector<render::DebugLine>* lines) const {
  if (!lines) return;
  Vec3 center;
  f32 radius = 0;
  u32 color = 0;
  if (editor_mode_ == EditorMode::kTerrain && terrain_cursor_hit_) {
    center = terrain_cursor_hit_->position;
    radius = terrain_brush_radius_;
    terrain::TerrainBrushMode mode = terrain_brush_mode_;
    if (window_->input().key(Key::kLeftShift)) {
      if (mode == terrain::TerrainBrushMode::kRaise) mode = terrain::TerrainBrushMode::kLower;
      else if (mode == terrain::TerrainBrushMode::kLower) mode = terrain::TerrainBrushMode::kRaise;
    }
    switch (mode) {
      case terrain::TerrainBrushMode::kRaise: color = 0x63d18aff; break;
      case terrain::TerrainBrushMode::kLower: color = 0xe0655fff; break;
      case terrain::TerrainBrushMode::kSmooth: color = 0x5a8deeff; break;
      case terrain::TerrainBrushMode::kFlatten: color = 0xe0b06aff; break;
      case terrain::TerrainBrushMode::kPaintLayer:
        if (terrain_brush_layer_ < terrain_.desc().layers.size())
          color = PackRgba(terrain_.desc().layers[terrain_brush_layer_].debug_rgba);
        break;
    }
  } else if (editor_mode_ == EditorMode::kPlace && placement_preview_) {
    center = *placement_preview_;
    radius = 0.32f;
    color = 0xe0b06aff;
  } else {
    return;
  }

  constexpr int kSegments = 40;
  Vec3 previous;
  for (int i = 0; i <= kSegments; ++i) {
    const f32 angle = static_cast<f32>(i) * 6.2831853f / kSegments;
    Vec3 point{center.x + std::cos(angle) * radius, center.y + 0.035f,
               center.z + std::sin(angle) * radius};
    if (editor_mode_ == EditorMode::kTerrain) {
      if (std::optional<f32> y = terrain_.SampleHeight(point.x, point.z)) point.y = *y + 0.035f;
    }
    if (i > 0) lines->push_back({previous, point, color});
    previous = point;
  }
  const f32 cross = std::max(0.18f, radius * 0.22f);
  lines->push_back({{center.x - cross, center.y + 0.045f, center.z},
                    {center.x + cross, center.y + 0.045f, center.z}, color});
  lines->push_back({{center.x, center.y + 0.045f, center.z - cross},
                    {center.x, center.y + 0.045f, center.z + cross}, color});
}

void Editor::LoadTerrainAsset(const std::string& path) {
  terrain::Terrain loaded;
  std::string error;
  if (!terrain::LoadTerrain(path, &loaded, &error)) {
    status_message_ = "Terrain load failed: " + error;
    RX_WARN("editor: {}", status_message_);
    MarkDirty();
    return;
  }
  FinishTerrainStroke();
  FinishPlacementDrag();
  ClearTerrainVisuals();
  terrain_ = std::move(loaded);
  terrain_path_ = fs::path(scene_path_).replace_extension(".rxterrain").string();
  terrain_dirty_ = true;  // activating an asset imports it into this scene
  terrain_brush_layer_ = 0;
  undo_.Clear();
  RebuildTerrainVisuals();
  SetEditorMode(EditorMode::kTerrain);
  status_message_ = "Activated " + fs::path(path).filename().string();
  RX_INFO("editor: loaded terrain {}", path);
  MarkDirty();
}

asset::AssetId Editor::ResolvePlacementMesh(const AssetEntry& entry) {
  if (entry.name == "cube.mesh") return cube_mesh_;
  if (entry.name == "sphere.mesh") return sphere_mesh_;
  if (entry.name == "plane.mesh") return plane_mesh_;
  if (auto cached = placement_meshes_.find(entry.path); cached != placement_meshes_.end())
    return cached->second;

  const std::string extension = fs::path(entry.path).extension().string();
  if (extension == ".gltf" || extension == ".glb") {
    asset::GltfScene scene;
    if (!asset::LoadGltfScene(entry.path, &scene) || scene.meshes.empty()) return {};
    const std::string source_path = asset::NormalizePath(entry.path);
    scene.meshes[0].id = asset::MakeAssetId(source_path);
    asset::RecordAssetPath(scene.meshes[0].id, source_path);
    if (renderer_) {
      for (const asset::Texture& texture : scene.textures) renderer_->UploadTexture(texture);
      for (const asset::Material& material : scene.materials) renderer_->UploadMaterial(material);
    }
    asset::AssetId first;
    for (asset::Mesh& mesh : scene.meshes) {
      if (!first) first = mesh.id;
      UploadPrimitive(entry.name, mesh);
    }
    placement_meshes_[entry.path] = first;
    return first;
  }

  if (assets_) {
    std::error_code error;
    std::string asset_path = fs::relative(entry.path, asset_root_, error).generic_string();
    if (error) asset_path = entry.path;
    if (const asset::Mesh* mesh = assets_->LoadMesh(asset_path)) {
      asset::AssetId id = UploadPrimitive(entry.name, *mesh);
      placement_meshes_[entry.path] = id;
      return id;
    }
  }
  return {};
}

void Editor::ArmPlacement(const AssetEntry& entry) {
  const asset::AssetId mesh = ResolvePlacementMesh(entry);
  if (!mesh) {
    status_message_ = "Could not load mesh " + entry.name;
    RX_WARN("editor: {}", status_message_);
    MarkDirty();
    return;
  }
  FinishTerrainStroke();
  FinishPlacementDrag();
  placement_.armed = true;
  placement_.mesh = mesh;
  placement_.name = entry.name;
  placement_.dragging = false;
  SetEditorMode(EditorMode::kPlace);
  selection_.Clear();
  pick_pending_ = false;
  status_message_ = "Placement brush armed: " + entry.name;
  MarkDirty();
}

}  // namespace rx::editor
