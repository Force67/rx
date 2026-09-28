#ifndef RX_EDITOR_APP_H_
#define RX_EDITOR_APP_H_

#include <stdint.h>

#include "foundation/math/math.h"
#include "rxe/anim/body_dynamics.h"
#include "rxe/anim/pose.h"
#include "rxe/anim/procedural_gait.h"
#include "rxe/app/application.h"
#include "rxe/asset/asset_database.h"
#include "rxe/asset/mesh.h"
#include "rxe/ui/events/input.h"

#include "base/containers/map.h"
#include "base/containers/pair.h"
#include "base/containers/span.h"
#include "base/containers/unordered_map.h"
#include "base/containers/vector.h"
#include "base/memory/move.h"
#include "base/optional.h"
#include "base/strings/xstring.h"
#include "plugins/terrain/terrain.h"
#include "rxe/edit/hierarchy.h"
#include "rxe/edit/reflect.h"
#include "rxe/edit/scene_io.h"
#include "rxe/edit/selection.h"
#include "rxe/edit/undo.h"
#include "rxe/scene/fly_camera.h"
#include "rxe/ui/ugui_backend.h"
#include "rxe/ui/ugui_platform.h"
#include "rxe/world/world_bake.h"

// libultragui
#include <ugui/ui_context.h>

namespace rx::editor {

// rx::foundation has no 2D vector type; the editor only needs one for screen-space
// (gizmo projection, cursor math), so define a small local one.
struct Vec2 {
  f32 x = 0;
  f32 y = 0;
};

enum class GizmoMode { kTranslate, kRotate, kScale };
enum class EditorMode { kSelect, kTerrain, kPlace };
enum class WalkPreviewMode { kAuto, kHipSway, kMarch };

// A content-browser entry discovered by scanning the mounted asset dir.
struct AssetEntry {
  base::String path; // built-in URI or source filesystem path
  base::String name; // basename
  base::String kind; // mesh/terrain/texture/material/audio/scene/model
};

// A CPU copy of an uploaded mesh, kept for ray-vs-triangle picking (the
// renderer does not read geometry back).
struct MeshRecord {
  asset::Mesh mesh;
  base::String name;
};

struct ImportedSkin {
  asset::Skeleton skeleton;
  asset::SkinBinding binding;
  anim::SkeletonPose pose;
  anim::BodyDynamics dynamics;
  base::Vector<anim::BodyMorphWeight> morphs;
  base::Vector<Mat4> model_matrices;
};

inline u64 ImportedEntityKey(ecs::Entity entity) {
  return static_cast<u64>(entity.generation) << 32 | entity.index;
}

struct ImportedInstance {
  ecs::Entity entity;
  u64 mesh = 0;
  i32 skin = -1;
  Vec3 turntable_position;
  Quat turntable_rotation;
  bool rotates_with_turntable = false;
  base::Vector<i32> remap;
  base::Vector<Mat4> palette;
  base::Vector<f32> morph_weights;
};

struct ImportedModel {
  base::String source_path;
  base::Vector<ImportedSkin> skins;
  base::Vector<ImportedInstance> instances;
  ecs::Entity turntable_entity;
  Vec3 turntable_center;
  f32 preview_time = 0;
  f32 walk_phase = 0;
  anim::WalkStyleKind active_walk_style = anim::WalkStyleKind::kHipSway;
  i32 force_event = -1;
};

// Active number-scrub (draggable field) state.
struct Scrub {
  bool active = false;
  ecs::Entity entity;
  const edit::ComponentDesc *comp = nullptr;
  const edit::PropDesc *prop = nullptr;
  int axis = 0;                  // 0..3 vector component
  f32 start_mouse = 0;           // mouse_x at grab
  f32 base_value = 0;            // value at grab
  f32 base_euler[3] = {0, 0, 0}; // for quat (rotation) scrubbing
  f32 step = 0.01f;
};

struct GizmoDrag {
  bool active = false;
  int axis = -1; // 0=x,1=y,2=z
  Vec3 base_pos{};
  f32 grab_mouse_x = 0, grab_mouse_y = 0;
  Vec2 axis_screen_dir{}; // normalized screen-space direction of the world axis
  f32 world_per_pixel = 0.01f;
};

struct TerrainTileVisual {
  terrain::TerrainTileKey key;
  ecs::Entity entity;
  asset::AssetId mesh;
};

inline base::Pair<i32, i32> TerrainTileMapKey(terrain::TerrainTileKey key) {
  return {key.x, key.z};
}

struct TerrainStroke {
  bool active = false;
  terrain::TerrainChange change;
  Vec3 last_dab{};
  f32 flatten_target = 0;
  base::String label;
};

struct PlacementBrush {
  bool armed = false;
  bool dragging = false;
  asset::AssetId mesh;
  base::String name;
  f32 spacing = 1.5f;
  Vec3 last_position{};
};

class Editor final : public app::Application {
public:
  explicit Editor(base::String open_path) : open_path_(base::move(open_path)) {}

  bool OnInitialize(app::Services &services) override;
  void OnUpdate(f32 dt) override;
  void OnBuildView(f32 dt, render::FrameView &view) override;
  void OnFrameEnd() override;
  void OnShutdown() override;

private:
  // scene / assets (editor_app.cc)
  void SetupDefaultScene();
  asset::AssetId UploadPrimitive(const base::String &name,
                                 const asset::Mesh &mesh);
  ecs::Entity SpawnMesh(const base::String &mesh_name, asset::AssetId mesh,
                        const Vec3 &pos, const base::String &label);
  void ScanAssets();
  bool LoadModelDocument(const base::String &path);
  void UpdateImportedModels(f32 dt);
  u32 ConfigureImportedBody(ImportedSkin *skin);

  // interaction (editor_app.cc)
  void UpdateCamera(f32 dt);
  bool CursorOverViewport() const;
  ecs::Entity PickAt(f32 mx, f32 my) const; // CPU raycast fallback
  void BeginScenePick(f32 mx, f32 my);      // engine GPU pick (async) or CPU
  void PollScenePick();                     // applies an arrived GPU result
  void FocusSelection();
  void UpdateScrub();
  bool TryStartScrub(f32 mx); // begin a number-field drag (editor_ui.cc)
  void UpdateGizmo(f32 mx, f32 my, bool lmb_down, bool lmb_edge);
  Vec2 ProjectToScreen(const Vec3 &world, bool *in_front) const;
  Mat4 ViewMatrix() const;
  Mat4 ProjMatrix() const;
  void PerformUndo();
  void PerformRedo();

  // terrain and surface placement (editor_terrain.cc)
  void SetupDefaultTerrain();
  void ClearTerrainVisuals();
  void RebuildTerrainVisuals();
  void RebuildTerrainTiles(base::Span<const terrain::TerrainTileKey> keys,
                           bool live);
  ecs::Entity SpawnTerrainTile(terrain::TerrainTileKey key,
                               asset::AssetId mesh);
  bool IsTerrainVisual(ecs::Entity entity) const;
  base::Pair<Vec3, Vec3> ViewportCameraRay(f32 mx, f32 my) const;
  void UpdateModeInteraction(bool lmb_down, bool lmb_edge);
  void FinishTerrainStroke();
  void FinishPlacementDrag();
  void SetEditorMode(EditorMode mode);
  void AppendInteractionPreview(base::Vector<render::DebugLine> *lines) const;
  void LoadTerrainAsset(const base::String &path);
  void ArmPlacement(const AssetEntry &asset);
  asset::AssetId ResolvePlacementMesh(const AssetEntry &asset);
  void RecordTerrainChange(terrain::TerrainChange change,
                           const base::String &label);
  void OnTerrainCommandReplayed(base::Span<const terrain::TerrainTileKey> keys);
  void SyncTerrainRayTracing(base::Span<const terrain::TerrainTileKey> keys);

  // file ops (editor_app.cc)
  void NewScene();
  void DoSave(const base::String &path);
  // Bake World: save first, then cook what was saved into <scene>.rxp. The cook
  // reads the file rather than this live world - the editor's world holds
  // transients the author never wrote (terrain tile visuals, preview models),
  // and edit::SaveScene already knows which of those to leave out.
  void DoBakeWorld();
  void DoLoad(const base::String &path);
  void OpenDocument(const base::String &path);
  void OpenFileDialog();
  void RunAutopilot(); // RX_EDITOR_AUTOPILOT smoke driver

  // ui (editor_ui.cc)
  bool UiInit();
  void UiShutdown();
  void UiFeedInput(f32 dt);
  void UiPerFrameText();
  void UiRebuild();
  base::String UiBuildDoc();
  base::String BuildHierarchy();
  base::String BuildInspector();
  base::String BuildContent();
  base::String BuildDialog();
  base::String BuildModeToolbar();
  base::String BuildInspectorTabs();
  base::String BuildTerrainInspector();
  base::String BuildPlacementInspector();
  void UiHotReloadCheck(f32 dt);
  void UpdateGizmoWidgets();
  void OnUiClick(ugui::wid w, ugui::MouseButton btn);
  void OnUiTextSubmit(const base::String &widget, const base::String &value);
  bool RouteClick(const base::String &name, ugui::MouseButton btn);
  void MarkDirty() { ui_dirty_ = true; }
  void SetDocDirty(bool d) { doc_dirty_ = d; }

  // helpers
  const MeshRecord *FindMesh(u64 hash) const;
  base::String EntityLabel(ecs::Entity e) const;

  app::Services *services_ = nullptr;
  app::Host *host_ = nullptr;
  Window *window_ = nullptr;
  render::Renderer *renderer_ = nullptr;
  ecs::World *world_ = nullptr;
  InputMap *input_map_ = nullptr;
  const ActionState *actions_ = nullptr;
  asset::Vfs *vfs_ = nullptr;
  bool headless_ = false;

  base::String open_path_; // scene/gltf passed on argv

  // editor state
  scene::FlyCamera camera_;
  edit::Selection selection_;
  edit::UndoStack undo_;
  base::Optional<asset::AssetDatabase> assets_; // constructed once vfs is known
  GizmoMode gizmo_mode_ = GizmoMode::kTranslate;
  EditorMode editor_mode_ = EditorMode::kSelect;
  Scrub scrub_;
  GizmoDrag gizmo_drag_;

  base::String scene_path_ = "untitled.rxscene";
  // Held once, so the Bake World action and the inspector's per-entity verdict
  // cannot disagree about the partition. The cell size in particular is not
  // scene data, and a label computed against a different one than the archive
  // was cooked with would be quietly wrong.
  world::WorldBakeOptions world_bake_options_;
  base::String terrain_path_ = "untitled.rxterrain";
  base::String asset_root_ = "assets";
  bool doc_dirty_ = false; // scene has unsaved changes
  bool terrain_dirty_ = false;
  bool terrain_command_replayed_ = false;
  base::String status_message_;
  bool playing_ = false;
  WalkPreviewMode walk_preview_mode_ = WalkPreviewMode::kAuto;
  bool material_tab_ = false;
  bool add_menu_open_ = false;
  bool dialog_open_ = false;
  base::Vector<base::String> dialog_files_;

  base::String search_filter_;
  base::String content_filter_;

  base::UnorderedMap<u64, MeshRecord> meshes_;
  asset::AssetId cube_mesh_, sphere_mesh_, plane_mesh_, terrain_material_;
  base::Vector<AssetEntry> assets_list_;
  base::UnorderedMap<base::String, asset::AssetId> placement_meshes_;
  base::Vector<ImportedModel> imported_models_;
  // Full entity handle -> (model index, instance index). Including the
  // generation prevents a reused ECS slot from inheriting stale skin state.
  base::UnorderedMap<u64, base::Pair<u32, u32>> imported_entities_;

  terrain::Terrain terrain_;
  // Ordered by (x, z): save respawns tiles in key order, which fixes their
  // entity ids. Keyed by a Pair because TerrainTileKey has no ordering.
  base::Map<base::Pair<i32, i32>, TerrainTileVisual> terrain_tiles_;
  terrain::TerrainBrushMode terrain_brush_mode_ =
      terrain::TerrainBrushMode::kRaise;
  f32 terrain_brush_radius_ = 2.0f;
  f32 terrain_brush_strength_ = 0.18f;
  f32 terrain_brush_falloff_ = 1.5f;
  u32 terrain_brush_layer_ = 0;
  TerrainStroke terrain_stroke_;
  PlacementBrush placement_;
  base::Optional<terrain::TerrainRayHit> terrain_cursor_hit_;
  base::Optional<Vec3> placement_preview_;

  // per-entity tint override (0 = none), for material-tint editing.
  base::UnorderedMap<u64, u32> tints_;

  // Display names live in scene::Name components (the ECS column-relocation
  // bug that corrupted base::String components is fixed on feature/editor-core).
  void SetName(ecs::Entity e, const base::String &name);
  base::String GetName(ecs::Entity e) const;

  // Engine GPU picking: pick_id -> entity map rebuilt each gather, plus the
  // in-flight request (results arrive 1-2 frames later).
  base::UnorderedMap<u32, ecs::Entity> pick_map_;
  bool pick_pending_ = false;

  // Debug-line storage for the frame (FrameView holds spans into these).
  base::Vector<render::DebugLine> grid_lines_;
  base::Vector<render::DebugLine> gizmo_lines_;

  // input edge tracking
  bool prev_lmb_ = false, prev_rmb_ = false;
  bool prev_key_[static_cast<int>(Key::kCount)] = {};

  // fps smoothing
  f32 fps_ = 0;

  ugui::UIContext ui_;
  ui::GuiRenderBackend backend_;
  ui::UguiHostState host_state_;
  ugui::FontHandle font_ = static_cast<ugui::FontHandle>(~0u);
  uint32_t font_revision_ = ~0u;
  const ugui::DrawData *draw_data_ = nullptr;
  bool ui_ready_ = false;
  bool ui_dirty_ = true; // widget tree needs a rebuild
  base::String ui_dir_;
  int64_t ui_mtime_ = 0;
  f32 reload_timer_ = 0;
};

} // namespace rx::editor

#endif // RX_EDITOR_APP_H_
