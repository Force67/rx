#ifndef RX_EDITOR_APP_H_
#define RX_EDITOR_APP_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/application.h"
#include "asset/asset_database.h"
#include "asset/mesh.h"
#include "core/input.h"
#include "core/math.h"

#include "edit/hierarchy.h"
#include "edit/reflect.h"
#include "edit/scene_io.h"
#include "edit/selection.h"
#include "edit/undo.h"
#include "fly_camera.h"
#include "ugui_backend.h"
#include "ugui_platform.h"

// libultragui
#include <ugui/ui_context.h>

namespace rx::editor {

// rx::core has no 2D vector type; the editor only needs one for screen-space
// (gizmo projection, cursor math), so define a small local one.
struct Vec2 {
  f32 x = 0;
  f32 y = 0;
};

enum class GizmoMode { kTranslate, kRotate, kScale };

// A content-browser entry discovered by scanning the mounted asset dir.
struct AssetEntry {
  std::string path;   // e.g. "assets://meshes/cube.mesh" (display)
  std::string name;   // basename
  std::string kind;   // "mesh" / "texture" / "material" / "audio" / "scene"
};

// A CPU copy of an uploaded mesh, kept for ray-vs-triangle picking (the renderer
// does not read geometry back).
struct MeshRecord {
  asset::Mesh mesh;
  std::string name;
};

// Active number-scrub (draggable field) state.
struct Scrub {
  bool active = false;
  ecs::Entity entity;
  const edit::ComponentDesc* comp = nullptr;
  const edit::PropDesc* prop = nullptr;
  int axis = 0;          // 0..3 vector component
  f32 start_mouse = 0;   // mouse_x at grab
  f32 base_value = 0;    // value at grab
  f32 base_euler[3] = {0, 0, 0};  // for quat (rotation) scrubbing
  f32 step = 0.01f;
};

// Active gizmo drag.
struct GizmoDrag {
  bool active = false;
  int axis = -1;         // 0=x,1=y,2=z
  Vec3 base_pos{};
  f32 grab_mouse_x = 0, grab_mouse_y = 0;
  Vec2 axis_screen_dir{};  // normalized screen-space direction of the world axis
  f32 world_per_pixel = 0.01f;
};

class Editor final : public app::Application {
 public:
  explicit Editor(std::string open_path) : open_path_(std::move(open_path)) {}

  bool OnInitialize(app::Services& services) override;
  void OnUpdate(f32 dt) override;
  void OnBuildView(f32 dt, render::FrameView& view) override;
  void OnFrameEnd() override;
  void OnShutdown() override;

 private:
  // --- scene / assets (editor_app.cc) ---
  void SetupDefaultScene();
  asset::AssetId UploadPrimitive(const std::string& name, const asset::Mesh& mesh);
  ecs::Entity SpawnMesh(const std::string& mesh_name, asset::AssetId mesh, const Vec3& pos,
                        const std::string& label);
  void ScanAssets();

  // --- interaction (editor_app.cc) ---
  void UpdateCamera(f32 dt);
  bool CursorOverViewport() const;
  ecs::Entity PickAt(f32 mx, f32 my) const;  // CPU raycast fallback
  void BeginScenePick(f32 mx, f32 my);       // engine GPU pick (async) or CPU
  void PollScenePick();                      // applies an arrived GPU result
  void FocusSelection();
  void UpdateScrub();
  bool TryStartScrub(f32 mx);  // begin a number-field drag (editor_ui.cc)
  void UpdateGizmo(f32 mx, f32 my, bool lmb_down, bool lmb_edge);
  Vec2 ProjectToScreen(const Vec3& world, bool* in_front) const;
  Mat4 ViewMatrix() const;
  Mat4 ProjMatrix() const;

  // --- file ops (editor_app.cc) ---
  void NewScene();
  void DoSave(const std::string& path);
  void DoLoad(const std::string& path);
  void OpenFileDialog();
  void RunAutopilot();  // RX_EDITOR_AUTOPILOT smoke driver

  // --- ui (editor_ui.cc) ---
  bool UiInit();
  void UiShutdown();
  void UiFeedInput(f32 dt);
  void UiPerFrameText();
  void UiRebuild();
  std::string UiBuildDoc();
  std::string BuildHierarchy();
  std::string BuildInspector();
  std::string BuildContent();
  std::string BuildDialog();
  void UiHotReloadCheck(f32 dt);
  void UpdateGizmoWidgets();
  void OnUiClick(ugui::wid w, ugui::MouseButton btn);
  void OnUiTextSubmit(const std::string& widget, const std::string& value);
  bool RouteClick(const std::string& name, ugui::MouseButton btn);
  void MarkDirty() { ui_dirty_ = true; }
  void SetDocDirty(bool d) { doc_dirty_ = d; }

  // helpers
  const MeshRecord* FindMesh(u64 hash) const;
  std::string EntityLabel(ecs::Entity e) const;

  // --- services ---
  app::Services* services_ = nullptr;
  app::Host* host_ = nullptr;
  Window* window_ = nullptr;
  render::Renderer* renderer_ = nullptr;
  ecs::World* world_ = nullptr;
  InputMap* input_map_ = nullptr;
  const ActionState* actions_ = nullptr;
  asset::Vfs* vfs_ = nullptr;
  bool headless_ = false;

  std::string open_path_;  // scene/gltf passed on argv

  // --- editor state ---
  FlyCamera camera_;
  edit::Selection selection_;
  edit::UndoStack undo_;
  std::optional<asset::AssetDatabase> assets_;  // constructed once vfs is known
  GizmoMode gizmo_mode_ = GizmoMode::kTranslate;
  Scrub scrub_;
  GizmoDrag gizmo_drag_;

  std::string scene_path_ = "untitled.rxscene";
  std::string asset_root_ = "assets";
  bool doc_dirty_ = false;  // scene has unsaved changes
  bool playing_ = false;
  bool material_tab_ = false;
  bool add_menu_open_ = false;
  bool dialog_open_ = false;
  std::vector<std::string> dialog_files_;

  std::string search_filter_;
  std::string content_filter_;

  std::unordered_map<uint64_t, MeshRecord> meshes_;
  asset::AssetId cube_mesh_, sphere_mesh_, plane_mesh_;
  std::vector<AssetEntry> assets_list_;

  // per-entity tint override (0 = none), for material-tint editing.
  std::unordered_map<uint64_t, uint32_t> tints_;

  // Display names live in scene::Name components (the ECS column-relocation
  // bug that corrupted std::string components is fixed on feature/editor-core).
  void SetName(ecs::Entity e, const std::string& name);
  std::string GetName(ecs::Entity e) const;

  // Engine GPU picking: pick_id -> entity map rebuilt each gather, plus the
  // in-flight request (results arrive 1-2 frames later).
  std::unordered_map<u32, ecs::Entity> pick_map_;
  bool pick_pending_ = false;

  // Debug-line storage for the frame (FrameView holds spans into these).
  std::vector<render::DebugLine> grid_lines_;
  std::vector<render::DebugLine> gizmo_lines_;

  // input edge tracking
  bool prev_lmb_ = false, prev_rmb_ = false;
  bool prev_key_[static_cast<int>(Key::kCount)] = {};

  // fps smoothing
  f32 fps_ = 0;

  // --- ugui ---
  ugui::UIContext ui_;
  ui::GuiRenderBackend backend_;
  ui::UguiHostState host_state_;
  ugui::FontHandle font_ = static_cast<ugui::FontHandle>(~0u);
  uint32_t font_revision_ = ~0u;
  const ugui::DrawData* draw_data_ = nullptr;
  bool ui_ready_ = false;
  bool ui_dirty_ = true;    // widget tree needs a rebuild
  std::string ui_dir_;
  int64_t ui_mtime_ = 0;
  f32 reload_timer_ = 0;
};

}  // namespace rx::editor

#endif  // RX_EDITOR_APP_H_
