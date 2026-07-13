#include "editor_app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "app/host.h"
#include "asset/primitives.h"
#include "asset/vfs.h"
#include "core/log.h"
#include "editor_input.h"
#include "edit/hierarchy.h"
#include "render/core/settings.h"
#include "scene/components.h"

namespace rx::editor {
namespace fs = std::filesystem;

namespace {
Mat4 MatOf(const scene::Transform& t) {
  return MakeTransform({t.position[0], t.position[1], t.position[2]},
                       Quat{t.rotation[0], t.rotation[1], t.rotation[2], t.rotation[3]}, t.scale);
}
constexpr f32 kTopBar = 44.0f;
constexpr f32 kViewportTop = 74.0f;   // topbar + viewport bar
constexpr f32 kLeftPanel = 300.0f;
constexpr f32 kRightPanel = 360.0f;
constexpr f32 kBottomPanels = 186.0f + 26.0f;  // content + statusbar
}  // namespace

// ===========================================================================
// Lifecycle
// ===========================================================================
bool Editor::OnInitialize(app::Services& s) {
  services_ = &s;
  host_ = s.host;
  window_ = s.window;
  renderer_ = s.renderer;
  world_ = s.world;
  input_map_ = s.input_map;
  actions_ = s.actions;
  vfs_ = s.vfs;
  headless_ = (window_ == nullptr);

  RegisterEditorInput(*input_map_);

  if (vfs_ && fs::exists(asset_root_))
    vfs_->Mount(asset::MakeLooseFileProvider(asset_root_));
  if (vfs_) assets_.emplace(*vfs_);

  if (renderer_) {
    renderer_->settings().ambient = 0.16f;  // lift shadows a touch for editing
  }

  SetupDefaultScene();

  // Load a scene passed on argv, if any (.rxscene only; .gltf is a known gap).
  if (!open_path_.empty()) {
    if (open_path_.size() > 8 && open_path_.substr(open_path_.size() - 8) == ".rxscene") {
      DoLoad(open_path_);
    } else {
      RX_WARN("editor: only .rxscene load is implemented (got {})", open_path_);
    }
  }

  camera_.set_position({6.0f, 4.5f, 6.0f});
  Vec3 d = Normalize(Vec3{0, 1.0f, 0} - camera_.position());
  camera_.set_yaw_pitch(std::atan2(d.x, -d.z), std::asin(std::clamp(d.y, -1.0f, 1.0f)));

  ScanAssets();

  if (!headless_) {
    if (!UiInit()) RX_WARN("editor: ugui init failed; running without UI");
  }
  return true;
}

void Editor::OnShutdown() {
  if (ui_ready_) UiShutdown();
}

// ===========================================================================
// Scene / assets
// ===========================================================================
void Editor::SetupDefaultScene() {
  // One neutral material; per-entity variety comes from DrawItem tint.
  asset::Material mat;
  mat.id = asset::MakeAssetId("editor/default_mat");
  mat.base_color_factor[0] = mat.base_color_factor[1] = mat.base_color_factor[2] = 1.0f;
  mat.metallic_factor = 0.0f;
  mat.roughness_factor = 0.7f;
  if (renderer_) renderer_->UploadMaterial(mat);

  auto assign = [&](asset::Mesh& m) {
    for (auto& lod : m.lods) {
      if (lod.submeshes.empty())
        lod.submeshes.push_back(asset::Submesh{0, (u32)lod.indices.size(), mat.id});
      else
        for (auto& sm : lod.submeshes) sm.material = mat.id;
    }
  };

  // Record id->path so .rxscene files serialize readable mesh references (the
  // procedural primitives are re-uploaded under the same ids every start).
  asset::RecordAssetPath(asset::MakeAssetId("editor/cube"), "editor/cube");
  asset::RecordAssetPath(asset::MakeAssetId("editor/sphere"), "editor/sphere");
  asset::RecordAssetPath(asset::MakeAssetId("editor/plane"), "editor/plane");

  asset::Mesh cube = asset::MakeCube(0.5f, asset::MakeAssetId("editor/cube"));
  assign(cube);
  cube_mesh_ = UploadPrimitive("cube.mesh", cube);

  asset::Mesh sphere = asset::MakeSphere(0.5f, 24, 32, asset::MakeAssetId("editor/sphere"));
  assign(sphere);
  sphere_mesh_ = UploadPrimitive("sphere.mesh", sphere);

  asset::Mesh plane = asset::MakeBox(6.0f, 0.05f, 6.0f, asset::MakeAssetId("editor/plane"));
  assign(plane);
  plane_mesh_ = UploadPrimitive("plane.mesh", plane);

  ecs::Entity ground = SpawnMesh("plane.mesh", plane_mesh_, {0, 0, 0}, "Ground");
  tints_[ground.index] = 0x3a3d42;
  ecs::Entity cube_e = SpawnMesh("cube.mesh", cube_mesh_, {-1.2f, 0.55f, 0}, "Cube");
  tints_[cube_e.index] = 0xc9a25a;
  ecs::Entity sphere_e = SpawnMesh("sphere.mesh", sphere_mesh_, {1.2f, 0.55f, 0}, "Sphere");
  tints_[sphere_e.index] = 0x6a9ad6;
  ecs::Entity cube2 = SpawnMesh("cube.mesh", cube_mesh_, {0, 0.55f, -1.6f}, "Prop");
  tints_[cube2.index] = 0x8ac98a;

  selection_.Set(cube_e);
}

asset::AssetId Editor::UploadPrimitive(const std::string& name, const asset::Mesh& mesh) {
  asset::Mesh copy = mesh;  // keep a CPU copy for picking
  if (renderer_) renderer_->UploadMesh(copy);
  meshes_[copy.id.hash] = MeshRecord{copy, name};
  return copy.id;
}

ecs::Entity Editor::SpawnMesh(const std::string& mesh_name, asset::AssetId mesh, const Vec3& pos,
                              const std::string& label) {
  ecs::Entity e = world_->Create();
  world_->Add(e, scene::Transform{.position = {pos.x, pos.y, pos.z}});
  world_->Add(e, scene::Renderable{mesh});
  edit::EnsureGuid(*world_, e);
  SetName(e, label);
  (void)mesh_name;
  return e;
}

void Editor::SetName(ecs::Entity e, const std::string& name) {
  if (scene::Name* n = world_->Get<scene::Name>(e))
    n->value = name;
  else
    world_->Add(e, scene::Name{name});
}

std::string Editor::GetName(ecs::Entity e) const {
  if (scene::Name* n = world_->Get<scene::Name>(e)) return n->value;
  return "";
}

void Editor::ScanAssets() {
  assets_list_.clear();
  // Built-in primitives always available.
  assets_list_.push_back({"assets://meshes/cube.mesh", "cube.mesh", "mesh"});
  assets_list_.push_back({"assets://meshes/sphere.mesh", "sphere.mesh", "mesh"});
  assets_list_.push_back({"assets://meshes/plane.mesh", "plane.mesh", "mesh"});

  // Loose files under the asset root, if it exists.
  if (fs::exists(asset_root_)) {
    for (auto& p : fs::recursive_directory_iterator(asset_root_)) {
      if (!p.is_regular_file()) continue;
      std::string ext = p.path().extension().string();
      std::string kind;
      if (ext == ".rxscene") kind = "scene";
      else if (ext == ".gltf" || ext == ".glb" || ext == ".mesh") kind = "mesh";
      else if (ext == ".png" || ext == ".jpg" || ext == ".dds" || ext == ".ktx") kind = "texture";
      else if (ext == ".mtl" || ext == ".mtlx" || ext == ".material") kind = "material";
      else if (ext == ".wav" || ext == ".ogg" || ext == ".xwm") kind = "audio";
      else continue;
      assets_list_.push_back({"assets://" + p.path().filename().string(),
                              p.path().filename().string(), kind});
    }
  }
}

const MeshRecord* Editor::FindMesh(u64 hash) const {
  auto it = meshes_.find(hash);
  return it == meshes_.end() ? nullptr : &it->second;
}

std::string Editor::EntityLabel(ecs::Entity e) const {
  std::string n = GetName(e);
  if (!n.empty()) return n;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "Entity %u", e.index);
  return buf;
}

// ===========================================================================
// Per-frame
// ===========================================================================
void Editor::OnUpdate(f32 dt) {
  if (!window_) return;
  const InputState& in = window_->input();

  f32 inst = dt > 0 ? 1.0f / dt : 0.0f;
  fps_ = fps_ <= 0 ? inst : (fps_ * 0.9f + inst * 0.1f);

  if (ui_ready_) {
    UiHotReloadCheck(dt);
    UiFeedInput(dt);
  }

  bool lmb = in.button(MouseButton::kLeft);
  bool lmb_edge = lmb && !prev_lmb_;
  bool over_vp = CursorOverViewport();

  static const bool input_log = std::getenv("RX_EDITOR_INPUT_LOG") != nullptr;
  if (input_log && lmb_edge)
    RX_INFO("editor: lmb down at {:.0f},{:.0f} over_vp={}", in.mouse_x, in.mouse_y, over_vp);

  // ---- gizmo / scrub / pick on the primary selection ----
  if (!headless_) {
    // Scrub start: LMB pressed over an inspector number field.
    if (lmb_edge && !scrub_.active && !gizmo_drag_.active) {
      TryStartScrub(in.mouse_x);  // begins a scrub if the cursor is over a number field
    }
    if (scrub_.active) {
      if (lmb) UpdateScrub();
      else { undo_.EndGroup(); scrub_.active = false; }
    }

    if (over_vp && !scrub_.active)
      UpdateGizmo(in.mouse_x, in.mouse_y, lmb, lmb_edge && !gizmo_drag_.active);

    // Pick when clicking empty viewport (no gizmo handle grabbed this click).
    if (lmb_edge && over_vp && !gizmo_drag_.active && !scrub_.active)
      BeginScenePick(in.mouse_x, in.mouse_y);
    PollScenePick();
  }

  UpdateCamera(dt);

  if (std::getenv("RX_EDITOR_AUTOPILOT")) RunAutopilot();

  // ---- keyboard shortcuts ----
  bool ctrl = in.key(Key::kLeftCtrl);
  auto edge = [&](Key k) {
    bool now = in.key(k);
    bool e = now && !prev_key_[(int)k];
    return e;
  };
  if (ctrl && edge(Key::kS)) { if (scene_path_ == "untitled.rxscene") OpenFileDialog(); else DoSave(scene_path_); }
  if (ctrl && edge(Key::kZ)) { undo_.Undo(*world_); MarkDirty(); doc_dirty_ = true; }
  // rx's Key enum has no Y/N/O; use available keys (R = redo, B = new scene, G = open).
  if (ctrl && edge(Key::kR)) { undo_.Redo(*world_); MarkDirty(); doc_dirty_ = true; }
  if (ctrl && edge(Key::kB)) NewScene();
  if (ctrl && edge(Key::kG)) OpenFileDialog();
  if (!ctrl && over_vp && edge(Key::kDelete)) {
    if (ecs::Entity e = selection_.primary()) {
      undo_.Push(*world_, edit::MakeDestroyEntity(*world_, e));
      selection_.Clear();
      MarkDirty();
      doc_dirty_ = true;
    }
  }
  if (!ctrl && over_vp && edge(Key::kF)) FocusSelection();

  for (int k = 0; k < (int)Key::kCount; ++k) prev_key_[k] = in.key((Key)k);
  prev_lmb_ = lmb;
  prev_rmb_ = in.button(MouseButton::kRight);
}

void Editor::UpdateCamera(f32 dt) {
  const InputState& in = window_->input();
  bool typing = false;  // camera only flies while RMB is held, so text fields are safe
  bool allow_mouse = CursorOverViewport() || camera_.looking();
  bool allow_keyboard = camera_.looking() && !typing;
  camera_.Update(in, *actions_, allow_mouse, allow_keyboard, dt);
  window_->SetRelativeMouseMode(camera_.looking());
}

bool Editor::CursorOverViewport() const {
  if (!window_) return false;
  const InputState& in = window_->input();
  f32 W = (f32)window_->width(), H = (f32)window_->height();
  return in.mouse_x >= kLeftPanel && in.mouse_x <= W - kRightPanel && in.mouse_y >= kViewportTop &&
         in.mouse_y <= H - kBottomPanels;
}

void Editor::FocusSelection() {
  ecs::Entity e = selection_.primary();
  if (!e) return;
  scene::Transform t = edit::WorldTransform(*world_, e);
  Vec3 center{t.position[0], t.position[1], t.position[2]};
  Vec3 eye = center - camera_.forward() * 4.0f;
  camera_.set_position(eye);
  Vec3 d = Normalize(center - eye);
  camera_.set_yaw_pitch(std::atan2(d.x, -d.z), std::asin(std::clamp(d.y, -1.0f, 1.0f)));
}

// ===========================================================================
// Picking. The engine GPU path (DrawItem::pick_id + Renderer::RequestPick /
// TakePickResult) is the default; the CPU ray-vs-mesh raycast stays compiled
// as the fallback behind the flag.
// ===========================================================================
#define RX_EDITOR_HAVE_ENGINE_PICKING 1

void Editor::BeginScenePick(f32 mx, f32 my) {
#if RX_EDITOR_HAVE_ENGINE_PICKING
  if (renderer_) {
    renderer_->RequestPick((u32)mx, (u32)my);
    pick_pending_ = true;
    return;
  }
#endif
  ecs::Entity hit = PickAt(mx, my);
  if (hit) selection_.Set(hit); else selection_.Clear();
  MarkDirty();
}

void Editor::PollScenePick() {
  if (!pick_pending_ || !renderer_) return;
  if (auto result = renderer_->TakePickResult()) {
    pick_pending_ = false;
    auto it = pick_map_.find(result->pick_id);
    if (it != pick_map_.end() && world_->IsAlive(it->second)) {
      selection_.Set(it->second);
      RX_INFO("editor: pick id {} -> {}", result->pick_id, EntityLabel(it->second));
    } else {
      selection_.Clear();  // background
      RX_INFO("editor: pick id {} -> background", result->pick_id);
    }
    MarkDirty();
  }
}

namespace {
bool RayTriangle(const Vec3& o, const Vec3& d, const Vec3& a, const Vec3& b, const Vec3& c,
                 f32* t_out) {
  Vec3 e1 = b - a, e2 = c - a;
  Vec3 p = Cross(d, e2);
  f32 det = Dot(e1, p);
  if (std::fabs(det) < 1e-8f) return false;
  f32 inv = 1.0f / det;
  Vec3 tv = o - a;
  f32 u = Dot(tv, p) * inv;
  if (u < 0 || u > 1) return false;
  Vec3 q = Cross(tv, e1);
  f32 v = Dot(d, q) * inv;
  if (v < 0 || u + v > 1) return false;
  f32 t = Dot(e2, q) * inv;
  if (t <= 1e-4f) return false;
  *t_out = t;
  return true;
}
}  // namespace

ecs::Entity Editor::PickAt(f32 mx, f32 my) const {
  f32 W = (f32)window_->width(), H = (f32)window_->height();
  f32 ndc_x = 2.0f * mx / W - 1.0f;
  f32 ndc_y = 1.0f - 2.0f * my / H;
  f32 aspect = W / H;
  f32 tan_half = std::tan(camera_.forward().y * 0 + 1.0472f * 0.5f);
  Vec3 fwd = camera_.forward();
  Vec3 right = Normalize(Cross(fwd, {0, 1, 0}));
  Vec3 up = Cross(right, fwd);
  Vec3 dir = Normalize(fwd + right * (ndc_x * aspect * tan_half) + up * (ndc_y * tan_half));
  Vec3 origin = camera_.position();

  f32 best_t = 1e30f;
  ecs::Entity best = ecs::kInvalidEntity;
  world_->Each<scene::Transform, scene::Renderable>(
      [&](ecs::Entity e, scene::Transform&, scene::Renderable& r) {
        if (world_->Has<scene::Hidden>(e)) return;
        const MeshRecord* rec = FindMesh(r.mesh.hash);
        if (!rec || rec->mesh.lods.empty()) return;
        scene::Transform wt = edit::WorldTransform(*world_, e);
        Mat4 world = MatOf(wt);
        // Broad phase: bounding sphere.
        Vec3 c = TransformPoint(world, {rec->mesh.bounds_center[0], rec->mesh.bounds_center[1],
                                        rec->mesh.bounds_center[2]});
        f32 radius = rec->mesh.bounds_radius * wt.scale * 1.05f + 0.05f;
        Vec3 oc = origin - c;
        f32 b = Dot(oc, dir);
        f32 cc = Dot(oc, oc) - radius * radius;
        if (cc > 0 && b > 0) return;
        if (b * b - cc < 0) return;
        // Narrow phase in local space (t stays the world-space distance).
        Mat4 inv = Inverse(world);
        Vec3 lo = TransformPoint(inv, origin);
        Vec3 ld = TransformDir(inv, dir);
        const auto& lod = rec->mesh.lods[0];
        for (size_t i = 0; i + 2 < lod.indices.size(); i += 3) {
          const auto& va = lod.vertices[lod.indices[i]];
          const auto& vb = lod.vertices[lod.indices[i + 1]];
          const auto& vc = lod.vertices[lod.indices[i + 2]];
          f32 t;
          if (RayTriangle(lo, ld, {va.position[0], va.position[1], va.position[2]},
                          {vb.position[0], vb.position[1], vb.position[2]},
                          {vc.position[0], vc.position[1], vc.position[2]}, &t)) {
            if (t < best_t) { best_t = t; best = e; }
          }
        }
      });
  return best;
}

// ===========================================================================
// Gizmo: 3D axis lines through FrameView::debug_lines_overlay + ugui handle
// dots (screen-space) for hit-testing.
// ===========================================================================
Mat4 Editor::ViewMatrix() const {
  return LookAt(camera_.position(), camera_.target(), {0, 1, 0});
}
Mat4 Editor::ProjMatrix() const {
  f32 aspect = (f32)window_->width() / (f32)window_->height();
  return PerspectiveReversedZ(1.0472f, aspect, 0.1f);
}

Vec2 Editor::ProjectToScreen(const Vec3& world, bool* in_front) const {
  Mat4 vp = ProjMatrix() * ViewMatrix();
  f32 x = vp.m[0] * world.x + vp.m[4] * world.y + vp.m[8] * world.z + vp.m[12];
  f32 y = vp.m[1] * world.x + vp.m[5] * world.y + vp.m[9] * world.z + vp.m[13];
  f32 w = vp.m[3] * world.x + vp.m[7] * world.y + vp.m[11] * world.z + vp.m[15];
  if (in_front) *in_front = w > 1e-4f;
  if (std::fabs(w) < 1e-6f) w = 1e-6f;
  f32 ndc_x = x / w, ndc_y = y / w;
  return {(ndc_x * 0.5f + 0.5f) * window_->width(), (ndc_y * 0.5f + 0.5f) * window_->height()};
}

void Editor::UpdateGizmo(f32 mx, f32 my, bool lmb_down, bool lmb_edge) {
  ecs::Entity e = selection_.primary();
  if (!e || gizmo_mode_ != GizmoMode::kTranslate) {
    gizmo_drag_.active = false;
    return;
  }
  scene::Transform* lt = world_->Get<scene::Transform>(e);
  if (!lt) return;
  scene::Transform wt = edit::WorldTransform(*world_, e);
  Vec3 origin{wt.position[0], wt.position[1], wt.position[2]};

  if (gizmo_drag_.active) {
    if (!lmb_down) {
      undo_.EndGroup();
      gizmo_drag_.active = false;
      return;
    }
    f32 dx = mx - gizmo_drag_.grab_mouse_x;
    f32 dy = my - gizmo_drag_.grab_mouse_y;
    f32 along = (dx * gizmo_drag_.axis_screen_dir.x + dy * gizmo_drag_.axis_screen_dir.y);
    f32 world_delta = along * gizmo_drag_.world_per_pixel;
    Vec3 axis{gizmo_drag_.axis == 0 ? 1.0f : 0.0f, gizmo_drag_.axis == 1 ? 1.0f : 0.0f,
              gizmo_drag_.axis == 2 ? 1.0f : 0.0f};
    Vec3 np = gizmo_drag_.base_pos + axis * world_delta;
    const edit::ComponentDesc* comp = edit::FindComponentByName("Transform");
    if (comp) {
      undo_.Push(*world_, edit::MakeSetProp(*world_, e, *comp, comp->props[0],
                                            edit::PropValue::Vec3(np.x, np.y, np.z)));
      doc_dirty_ = true;
    }
    return;
  }

  // Not dragging: on click, test the three axis-handle tips.
  if (lmb_edge) {
    f32 len = std::max(0.5f, Length(origin - camera_.position()) * 0.18f);
    for (int a = 0; a < 3; ++a) {
      Vec3 axis{a == 0 ? 1.0f : 0.0f, a == 1 ? 1.0f : 0.0f, a == 2 ? 1.0f : 0.0f};
      bool f0, f1;
      Vec2 s0 = ProjectToScreen(origin, &f0);
      Vec2 s1 = ProjectToScreen(origin + axis * len, &f1);
      if (!f1) continue;
      // distance from cursor to the handle tip
      f32 d = std::hypot(mx - s1.x, my - s1.y);
      if (d < 14.0f) {
        gizmo_drag_.active = true;
        gizmo_drag_.axis = a;
        gizmo_drag_.base_pos = Vec3{lt->position[0], lt->position[1], lt->position[2]};
        gizmo_drag_.grab_mouse_x = mx;
        gizmo_drag_.grab_mouse_y = my;
        Vec2 sd{s1.x - s0.x, s1.y - s0.y};
        f32 sl = std::max(1.0f, std::hypot(sd.x, sd.y));
        gizmo_drag_.axis_screen_dir = {sd.x / sl, sd.y / sl};
        gizmo_drag_.world_per_pixel = len / sl;
        undo_.BeginGroup("Move");
        return;
      }
    }
  }
}

// ===========================================================================
// File ops
// ===========================================================================
void Editor::NewScene() {
  std::vector<ecs::Entity> all;
  world_->Each<scene::Transform>([&](ecs::Entity e, scene::Transform&) { all.push_back(e); });
  for (ecs::Entity e : all) world_->Destroy(e);
  undo_.Clear();
  selection_.Clear();
  tints_.clear();
  SetupDefaultScene();
  scene_path_ = "untitled.rxscene";
  doc_dirty_ = false;
  MarkDirty();
}

void Editor::DoSave(const std::string& path) {
  std::string err;
  if (edit::SaveScene(*world_, path, &err)) {
    scene_path_ = path;
    doc_dirty_ = false;
    RX_INFO("editor: saved {}", path);
  } else {
    RX_WARN("editor: save failed: {}", err);
  }
  MarkDirty();
}

void Editor::DoLoad(const std::string& path) {
  if (!assets_) {
    RX_WARN("editor: no asset database (headless vfs?); cannot load scenes");
    return;
  }
  // The engine LoadScene leaves existing entities untouched; replacing the open
  // document is editor policy, so clear the current scene entities first.
  std::vector<ecs::Entity> old;
  world_->Each<scene::Transform>([&](ecs::Entity e, scene::Transform&) { old.push_back(e); });
  std::string err;
  if (edit::LoadScene(*world_, *assets_, path, &err)) {
    for (ecs::Entity e : old) world_->Destroy(e);
    scene_path_ = path;
    doc_dirty_ = false;
    undo_.Clear();
    selection_.Clear();
    tints_.clear();
    RX_INFO("editor: loaded {}", path);
  } else {
    RX_WARN("editor: load failed: {}", err);
  }
  MarkDirty();
}

void Editor::OpenFileDialog() {
  dialog_files_.clear();
  if (fs::exists(asset_root_)) {
    for (auto& p : fs::recursive_directory_iterator(asset_root_))
      if (p.is_regular_file() && p.path().extension() == ".rxscene")
        dialog_files_.push_back(p.path().string());
  }
  // Also any .rxscene in the working dir.
  for (auto& p : fs::directory_iterator(fs::current_path()))
    if (p.is_regular_file() && p.path().extension() == ".rxscene")
      dialog_files_.push_back(p.path().filename().string());
  dialog_open_ = true;
  MarkDirty();
}

// ===========================================================================
// Autopilot (RX_EDITOR_AUTOPILOT=1): drives the editor's own interaction code
// paths at fixed frames -- GPU pick round-trips at projected entity pixels,
// an undo-grouped move (the gizmo-drag path), undo/redo, and a save/new/load
// round-trip -- logging pass/fail so a headless GPU run smoke-tests the whole
// engine-integration surface without OS-synthesized input.
// ===========================================================================
void Editor::RunAutopilot() {
  static int f = 0;
  ++f;
  auto find_named = [&](const char* n) {
    ecs::Entity found = ecs::kInvalidEntity;
    world_->Each<scene::Name>([&](ecs::Entity e, scene::Name& nm) {
      if (nm.value == n) found = e;
    });
    return found;
  };
  auto pick_at_entity = [&](const char* n) {
    ecs::Entity e = find_named(n);
    if (!e) { RX_WARN("autopilot: no entity '{}'", n); return; }
    scene::Transform wt = edit::WorldTransform(*world_, e);
    bool in_front;
    Vec2 px = ProjectToScreen({wt.position[0], wt.position[1], wt.position[2]}, &in_front);
    RX_INFO("autopilot: RequestPick at {:.0f},{:.0f} (center of '{}')", px.x, px.y, n);
    BeginScenePick(px.x, px.y);
  };
  auto check_sel = [&](const char* expect) {
    std::string got = selection_.primary() ? EntityLabel(selection_.primary()) : "(none)";
    RX_INFO("autopilot: selection = '{}' expected '{}' -> {}", got, expect,
            got == expect ? "PASS" : "FAIL");
  };

  const edit::ComponentDesc* xf = edit::FindComponentByName("Transform");
  switch (f) {
    case 100: pick_at_entity("Cube"); break;
    case 160: check_sel("Cube"); break;
    case 200: pick_at_entity("Sphere"); break;
    case 260: check_sel("Sphere"); break;
    case 300:
      RX_INFO("autopilot: RequestPick at 700,150 (sky)");
      BeginScenePick(700, 150);
      break;
    case 360: check_sel("(none)"); break;
    case 400: {  // undo-grouped move of the Cube (the gizmo-drag path)
      ecs::Entity e = find_named("Cube");
      selection_.Set(e);
      undo_.BeginGroup("Move");
      undo_.Push(*world_, edit::MakeSetProp(*world_, e, *xf, xf->props[0],
                                            edit::PropValue::Vec3(-1.2f, 0.55f, 1.0f)));
      undo_.Push(*world_, edit::MakeSetProp(*world_, e, *xf, xf->props[0],
                                            edit::PropValue::Vec3(-1.2f, 0.55f, 2.0f)));
      undo_.EndGroup();
      doc_dirty_ = true;
      MarkDirty();
      RX_INFO("autopilot: moved Cube to z=2 (grouped)");
      break;
    }
    case 430: {
      undo_.Undo(*world_);
      scene::Transform* t = world_->Get<scene::Transform>(find_named("Cube"));
      RX_INFO("autopilot: undo -> cube z={:.2f} expected 0 -> {}", t ? t->position[2] : -99.f,
              (t && std::fabs(t->position[2]) < 1e-3f) ? "PASS" : "FAIL");
      MarkDirty();
      break;
    }
    case 460: {
      undo_.Redo(*world_);
      scene::Transform* t = world_->Get<scene::Transform>(find_named("Cube"));
      RX_INFO("autopilot: redo -> cube z={:.2f} expected 2 -> {}", t ? t->position[2] : -99.f,
              (t && std::fabs(t->position[2] - 2.0f) < 1e-3f) ? "PASS" : "FAIL");
      MarkDirty();
      break;
    }
    case 500: DoSave("scene_saved.rxscene"); break;
    case 540: NewScene(); RX_INFO("autopilot: new scene, {} entities", world_->entity_count()); break;
    case 580: DoLoad("scene_saved.rxscene"); break;
    case 620: {
      ecs::Entity e = find_named("Cube");
      scene::Transform* t = e ? world_->Get<scene::Transform>(e) : nullptr;
      RX_INFO("autopilot: after load cube z={:.2f} expected 2 -> {}", t ? t->position[2] : -99.f,
              (t && std::fabs(t->position[2] - 2.0f) < 1e-3f) ? "PASS" : "FAIL");
      break;
    }
    case 660: pick_at_entity("Sphere"); break;
    case 720: check_sel("Sphere"); RX_INFO("autopilot: done"); break;
    default: break;
  }
}

void Editor::OnFrameEnd() {
  // Headless-driving hooks for smoke tests:
  //   RX_EDITOR_SHOT=path        capture once at frame RX_EDITOR_SHOT_FRAME (20)
  //   RX_EDITOR_SHOT_EVERY=n     capture path.<k>.png every n frames instead
  //   RX_EDITOR_SHOT_QUIT=1      quit a few frames after the single capture
  //   RX_EDITOR_QUIT_FRAME=n     quit at frame n
  static int frames = 0;
  ++frames;
  if (const char* p = std::getenv("RX_EDITOR_SHOT"); p && renderer_) {
    int every = std::getenv("RX_EDITOR_SHOT_EVERY") ? std::atoi(std::getenv("RX_EDITOR_SHOT_EVERY")) : 0;
    int at = std::getenv("RX_EDITOR_SHOT_FRAME") ? std::atoi(std::getenv("RX_EDITOR_SHOT_FRAME")) : 20;
    if (every > 0) {
      if (frames % every == 0) {
        char path[1024];
        std::snprintf(path, sizeof(path), "%s.%03d.png", p, frames / every);
        renderer_->CaptureScreenshot(path);
      }
    } else {
      if (frames == at) renderer_->CaptureScreenshot(p);
      if (frames == at + 4 && std::getenv("RX_EDITOR_SHOT_QUIT")) host_->RequestQuit();
    }
  }
  if (const char* q = std::getenv("RX_EDITOR_QUIT_FRAME")) {
    if (frames >= std::atoi(q)) host_->RequestQuit();
  }
}

}  // namespace rx::editor
