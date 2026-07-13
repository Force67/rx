// The editor's ugui layer: draw-data-mode UIContext driven by the recreation-
// style GuiRenderBackend through FrameView::hud_draw, plus the C++ that
// generates the .ugui document from editor state and routes widget events.
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>

#include <ugui/core/color.h>
#include <ugui/style/style.h>
#include <ugui/widgets/button.h>
#include <ugui/widgets/text.h>
#include <ugui/widgets/text_input.h>
#include <ugui/widgets/widget.h>
#include <ugui/widgets/widget_registry.h>

#include "core/log.h"
#include "editor_app.h"
#include "edit/hierarchy.h"
#include "render/rhi/vulkan_interop.h"
#include "scene/components.h"

namespace rx::editor {
namespace fs = std::filesystem;

namespace {
const char* FindFont() {
  static std::string resolved;
  if (FILE* p = popen("fc-match -f '%{file}' sans 2>/dev/null", "r")) {
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, p);
    pclose(p);
    if (n > 0) {
      buf[n] = '\0';
      resolved = buf;
      if (fs::exists(resolved)) return resolved.c_str();
    }
  }
  static const char* candidates[] = {
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/TTF/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
      "/run/current-system/sw/share/X11/fonts/DejaVuSans.ttf",
  };
  for (auto* c : candidates)
    if (fs::exists(c)) return c;
  return nullptr;
}

std::string F(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return buf;
}

// Quaternion -> euler XYZ degrees (for the rotation row).
void QuatToEuler(const Quat& q, f32 out_deg[3]) {
  f32 sinr = 2 * (q.w * q.x + q.y * q.z);
  f32 cosr = 1 - 2 * (q.x * q.x + q.y * q.y);
  f32 x = std::atan2(sinr, cosr);
  f32 sinp = 2 * (q.w * q.y - q.z * q.x);
  f32 y = std::fabs(sinp) >= 1 ? std::copysign(1.5707963f, sinp) : std::asin(sinp);
  f32 siny = 2 * (q.w * q.z + q.x * q.y);
  f32 cosy = 1 - 2 * (q.y * q.y + q.z * q.z);
  f32 z = std::atan2(siny, cosy);
  const f32 r2d = 57.29578f;
  out_deg[0] = x * r2d;
  out_deg[1] = y * r2d;
  out_deg[2] = z * r2d;
}
Quat EulerToQuat(const f32 deg[3]) {
  const f32 d2r = 0.0174533f;
  f32 cx = std::cos(deg[0] * d2r * 0.5f), sx = std::sin(deg[0] * d2r * 0.5f);
  f32 cy = std::cos(deg[1] * d2r * 0.5f), sy = std::sin(deg[1] * d2r * 0.5f);
  f32 cz = std::cos(deg[2] * d2r * 0.5f), sz = std::sin(deg[2] * d2r * 0.5f);
  return Normalize(Quat{sx * cy * cz - cx * sy * sz, cx * sy * cz + sx * cy * sz,
                        cx * cy * sz - sx * sy * cz, cx * cy * cz + sx * sy * sz});
}

// Engine reflection registers lowercase prop names ("position"); display them
// capitalized in the inspector.
std::string DisplayName(const char* n) {
  std::string s = n ? n : "";
  if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = (char)(s[0] - 'a' + 'A');
  return s;
}

const char* AxisLabel(int i) { return i == 0 ? "X" : i == 1 ? "Y" : i == 2 ? "Z" : "W"; }
const char* AxisColor(int i) {
  return i == 0 ? "#e0655f" : i == 1 ? "#7fb96a" : i == 2 ? "#5a8dee" : "#c9a25a";
}
std::string KindColor(const std::string& k) {
  if (k == "mesh") return "#2f7f6a";
  if (k == "texture") return "#9a7f3a";
  if (k == "material") return "#6a4fbf";
  if (k == "scene") return "#bf5a7f";
  if (k == "audio") return "#5a7fbf";
  return "#4a4d52";
}
}  // namespace

// ===========================================================================
// ugui init / shutdown
// ===========================================================================
bool Editor::UiInit() {
  host_state_.window_width = (f32)window_->width();
  host_state_.window_height = (f32)window_->height();

  ugui::UIConfig cfg;
  cfg.draw_data = true;
  cfg.external_window = &host_state_;
  cfg.width = window_->width();
  cfg.height = window_->height();
  if (!ui_.Init(cfg)) return false;

  if (const char* font = FindFont())
    ui_.set_default_font(font_ = ui_.LoadFont(font));
  else
    RX_WARN("editor: no system font found; text will not render");

  render::VulkanHandles vk = render::GetVulkanHandles(*renderer_->device());
  if (!vk.device) {
    RX_WARN("editor: renderer is not on the Vulkan backend; ugui disabled");
    return false;
  }
  ui::GuiRenderBackend::InitInfo bi;
  bi.instance = vk.instance;
  bi.physical_device = vk.physical_device;
  bi.device = vk.device;
  bi.queue_family = vk.graphics_family;
  bi.queue = vk.graphics_queue;
  bi.color_format = render::GetVkFormat(renderer_->swapchain_format());
  bi.frames_in_flight = render::GetVulkanFramesInFlight(*renderer_->device());
  if (!backend_.Init(bi)) return false;
  ui_.set_texture_backend(&backend_);

  ui_.input().set_on_click([this](ugui::wid w, ugui::MouseButton b) { OnUiClick(w, b); });

  // Locate the .ugui source dir (compile-def default, env override).
#ifdef RX_EDITOR_UI_DIR
  ui_dir_ = RX_EDITOR_UI_DIR;
#endif
  if (const char* env = std::getenv("RX_EDITOR_UI_DIR")) ui_dir_ = env;

  ui_ready_ = true;
  UiRebuild();
  return true;
}

void Editor::UiShutdown() {
  backend_.Shutdown();
  ui_.Shutdown();
  ui_ready_ = false;
}

// ===========================================================================
// Input feeding
// ===========================================================================
void Editor::UiFeedInput(f32) {
  const InputState& in = window_->input();
  host_state_.window_width = (f32)window_->width();
  host_state_.window_height = (f32)window_->height();

  ugui::InputQueue& q = ui_.platform()->input_queue();
  q.PushMove({in.mouse_x, in.mouse_y});

  const ugui::MouseButton ub[3] = {ugui::MouseButton::kLeft, ugui::MouseButton::kRight,
                                   ugui::MouseButton::kMiddle};
  const MouseButton rb[3] = {MouseButton::kLeft, MouseButton::kRight, MouseButton::kMiddle};
  static bool prev[3] = {false, false, false};
  for (int i = 0; i < 3; ++i) {
    bool down = in.button(rb[i]);
    if (down != prev[i]) q.PushButton(ub[i], down);
    prev[i] = down;
  }
  if (in.wheel != 0) q.PushScroll({0, in.wheel});

  // Text + editing keys (GLFW codes, as the ugui text-input expects).
  int shift = in.key(Key::kLeftShift) ? 0x0001 : 0;
  auto key_edge = [&](Key k) { return in.key_pressed(k); };
  if (key_edge(Key::kBackspace)) q.PushKey(259, 0, true, false, shift);
  if (key_edge(Key::kReturn)) q.PushKey(257, 0, true, false, shift);
  if (key_edge(Key::kTab)) q.PushKey(258, 0, true, false, shift);
  if (key_edge(Key::kDelete)) q.PushKey(261, 0, true, false, shift);
  if (key_edge(Key::kArrowLeft)) q.PushKey(263, 0, true, false, shift);
  if (key_edge(Key::kArrowRight)) q.PushKey(262, 0, true, false, shift);
  // UTF-8 text this pump -> codepoints.
  for (u8 i = 0; i < in.text_len;) {
    unsigned char c = (unsigned char)in.text[i];
    u32 cp = 0, n = 1;
    if (c < 0x80) { cp = c; n = 1; }
    else if ((c >> 5) == 0x6) { cp = c & 0x1F; n = 2; }
    else if ((c >> 4) == 0xE) { cp = c & 0x0F; n = 3; }
    else if ((c >> 3) == 0x1E) { cp = c & 0x07; n = 4; }
    for (u32 k = 1; k < n && i + k < in.text_len; ++k) cp = (cp << 6) | (in.text[i + k] & 0x3F);
    if (cp >= 32) q.PushChar(cp);
    i += n;
  }
}

// ===========================================================================
// Per-frame text + gizmo widget updates
// ===========================================================================
void Editor::UiPerFrameText() {
  auto set = [&](const char* name, const std::string& v) {
    ugui::wid w = ui_.FindWidget(name);
    if (w.valid()) ugui::SetText(w, v);
  };
  set("status_fps", F("%.0f FPS", fps_));
  set("status_path", scene_path_);
  set("status_dirty", doc_dirty_ ? "* modified" : "");
  set("status_count", F("%zu entities", world_->entity_count()));
  set("btn_undo", undo_.can_undo() ? F("Undo: %s", undo_.undo_label()) : "Undo");
  set("btn_redo", undo_.can_redo() ? F("Redo: %s", undo_.redo_label()) : "Redo");
  set("vp_gizmo", gizmo_mode_ == GizmoMode::kTranslate ? "Gizmo: Move"
                  : gizmo_mode_ == GizmoMode::kRotate  ? "Gizmo: Rotate"
                                                       : "Gizmo: Scale");
}

void Editor::UpdateGizmoWidgets() {
  ecs::Entity e = selection_.primary();
  bool show = e && gizmo_mode_ == GizmoMode::kTranslate && CursorOverViewport() == CursorOverViewport();
  scene::Transform wt = e ? edit::WorldTransform(*world_, e) : scene::Transform{};
  Vec3 origin{wt.position[0], wt.position[1], wt.position[2]};
  f32 len = e ? std::max(0.5f, Length(origin - camera_.position()) * 0.18f) : 0;
  for (int a = 0; a < 3; ++a) {
    ugui::wid w = ui_.FindWidget(F("gizmo_%c", "xyz"[a]).c_str());
    if (!w.valid()) continue;
    ugui::StyleC* sc = ui_.world().Get<ugui::StyleC>(w);
    if (!sc) continue;
    ugui::Style s = sc->style;
    if (!e) {
      s.visibility = ugui::Visibility::kHidden;
    } else {
      Vec3 axis{a == 0 ? 1.0f : 0.0f, a == 1 ? 1.0f : 0.0f, a == 2 ? 1.0f : 0.0f};
      bool infront;
      Vec2 tip = ProjectToScreen(origin + axis * len, &infront);
      s.visibility = infront ? ugui::Visibility::kVisible : ugui::Visibility::kHidden;
      s.position = ugui::Position::kAbsolute;
      s.left_offset = ugui::Length::Px(tip.x - 7.0f);
      s.top = ugui::Length::Px(tip.y - 7.0f);
    }
    ugui::SetStyle(ui_.world(), w, s);
  }
}

// ===========================================================================
// Document generation
// ===========================================================================
void Editor::UiRebuild() {
  if (!ui_ready_) return;
  std::string doc = UiBuildDoc();
  ui_.LoadUiString(doc.c_str(), "editor");
  ui_.InvalidateWidgetCache();

  // Wire text-input handlers (they survive until the next rebuild).
  if (ugui::wid s = ui_.FindWidget("hier_search"); s.valid()) {
    ugui::SetTextInputValue(s, search_filter_);
    ugui::SetTextInputChange(s, [this](const ugui::String& v) {
      search_filter_ = v;
      MarkDirty();
    });
  }
  if (ugui::wid c = ui_.FindWidget("content_filter"); c.valid()) {
    ugui::SetTextInputValue(c, content_filter_);
    ugui::SetTextInputChange(c, [this](const ugui::String& v) {
      content_filter_ = v;
      MarkDirty();
    });
  }
  if (ugui::wid n = ui_.FindWidget("insp_name"); n.valid()) {
    if (ecs::Entity e = selection_.primary(); e && world_->IsAlive(e))
      ugui::SetTextInputValue(n, EntityLabel(e));
    ugui::SetTextInputSubmit(n, [this](const ugui::String& v) {
      OnUiTextSubmit("insp_name", v);
    });
  }
  ui_.input().RefreshHover(ui_.root());
  UiPerFrameText();
  ui_dirty_ = false;
}

std::string Editor::UiBuildDoc() {
  std::string tmpl;
  std::string path = (ui_dir_.empty() ? std::string("apps/editor/ui") : ui_dir_) + "/editor.ugui";
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) tmpl.append(buf, n);
    std::fclose(f);
    ui_mtime_ = (int64_t)fs::last_write_time(path).time_since_epoch().count();
  } else {
    RX_WARN("editor: cannot read {}", path);
    return "panel root { background: #101113; text t { text: \"missing editor.ugui\"; color:#f00; } }";
  }
  auto replace = [&](const char* marker, const std::string& with) {
    size_t p = tmpl.find(marker);
    if (p != std::string::npos) tmpl.replace(p, std::strlen(marker), with);
  };
  replace("//@@HIERARCHY@@", BuildHierarchy());
  replace("//@@INSPECTOR@@", BuildInspector());
  replace("//@@CONTENT@@", BuildContent());
  replace("//@@GIZMO@@", "panel gizmo_x { position: absolute; width: 14; height: 14; background: #e0655f; corner-radius: 7; border-color:#ffffff; border-width:1; }\n"
                          "panel gizmo_y { position: absolute; width: 14; height: 14; background: #7fb96a; corner-radius: 7; border-color:#ffffff; border-width:1; }\n"
                          "panel gizmo_z { position: absolute; width: 14; height: 14; background: #5a8dee; corner-radius: 7; border-color:#ffffff; border-width:1; }\n");
  replace("//@@DIALOG@@", BuildDialog());
  return tmpl;
}

std::string Editor::BuildHierarchy() {
  std::string out;
  // Recursive emit respecting Parent; filter by search.
  std::function<void(ecs::Entity, int)> emit = [&](ecs::Entity e, int depth) {
    std::string label = EntityLabel(e);
    bool match = search_filter_.empty() ||
                 label.find(search_filter_) != std::string::npos;
    if (match) {
      bool sel = selection_.Contains(e);
      const char* icon = world_->Has<scene::Renderable>(e) ? "#e0b06a" : "#c78ad6";
      out += F("button hier_%u_%u { layout: row; align: center; height: 26; gap: 8; "
               "padding: 0 6 0 %d; corner-radius: 5; cursor: pointer; %s "
               ":hover { background: #2a2d33; }\n",
               e.index, e.generation, 8 + depth * 16,
               sel ? "background: #2f4368; border-color: #5a8dee; border-width: 0 0 0 2;" : "");
      out += F("  panel d { width: 8; height: 8; corner-radius: 4; background: %s; }\n", icon);
      out += F("  text l { text: \"%s\"; font-size: 13; color: %s; }\n", label.c_str(),
               sel ? "#ffffff" : "#c9ccd2");
      out += "}\n";
    }
    // children
    world_->Each<scene::Parent>([&](ecs::Entity c, scene::Parent& p) {
      if (p.value == e) emit(c, depth + 1);
    });
  };
  world_->Each<scene::Transform>([&](ecs::Entity e, scene::Transform&) {
    if (!world_->Has<scene::Parent>(e)) emit(e, 0);
  });
  if (out.empty()) out = "text empty { text: \"(no entities)\"; font-size: 12; color: #5a5d63; padding: 8; }\n";
  return out;
}

std::string Editor::BuildInspector() {
  ecs::Entity e = selection_.primary();
  if (!e || !world_->IsAlive(e))
    return "text nosel { text: \"No selection\"; font-size: 13; color: #6a6d73; padding: 16; }\n";

  std::string out;
  // Name row.
  out += "panel name_row { layout: row; align: center; padding: 10 12; gap: 8; border-color:#101113; border-width:0 0 1 0;\n";
  out += "  panel ni { width: 8; height: 8; corner-radius: 4; background: #e0b06a; }\n";
  out += F("  text-input insp_name { value: \"%s\"; font-size: 13; color: #eceef2; "
           "background: #191a1d; corner-radius: 6; border-color: #33363c; border-width: 1; "
           "padding: 6 10; flex-grow: 1; tab-index: 5; }\n",
           EntityLabel(e).c_str());
  out += "}\n";

  if (material_tab_) {
    // Material tab: submesh materials + a live tint override.
    out += "panel mat_head { class: comp_header; text mt { class: comp_title; text: \"Material\"; } }\n";
    scene::Renderable* r = world_->Get<scene::Renderable>(e);
    const MeshRecord* rec = r ? FindMesh(r->mesh.hash) : nullptr;
    int subs = 0;
    if (rec && !rec->mesh.lods.empty()) subs = (int)rec->mesh.lods[0].submeshes.size();
    out += F("panel matb { layout: column; padding: 10 12; gap: 8;\n"
             "  panel mrow { layout: row; gap: 8; align:center; text ml { class: row_label; text: \"Mesh\"; } "
             "panel mv { class: field; text mvv { class: field_val; color:#8fa9d6; text: \"%s\"; } } }\n"
             "  panel srow { layout: row; gap: 8; align:center; text sl { class: row_label; text: \"Submeshes\"; } "
             "panel sv { class: field; text svv { class: field_val; text: \"%d\"; } } }\n",
             rec ? rec->name.c_str() : "(none)", subs);
    uint32_t tint = tints_.count(e.index) ? tints_[e.index] : 0xffffff;
    out += F("  panel trow { layout: row; gap: 8; align:center; text tl { class: row_label; text: \"Tint\"; }\n"
             "    panel tsw { width: 26; height: 26; corner-radius: 5; border-color:#33363c; border-width:1; background: #%06x; }\n",
             tint);
    for (int a = 0; a < 3; ++a) {
      int v = (tint >> (16 - a * 8)) & 0xFF;
      out += F("    panel field_tint_0_%d { class: field; text fk { font-size:11; font-weight:bold; color:%s; text:\"%s\"; } "
               "text fv { class: field_val; text: \"%d\"; } }\n",
               a, AxisColor(a), a == 0 ? "R" : a == 1 ? "G" : "B", v);
    }
    out += "  }\n}\n";
    out += "  panel matnote { padding: 4 12; text mn { text: \"Tint is a live per-draw base-color override (DrawItem.tint).\"; font-size: 11; color:#6a6d73; } }\n";
    return out;
  }

  // Inspector tab: one section per component.
  for (const edit::ComponentDesc* comp : edit::ComponentsOn(*world_, e)) {
    if (std::string(comp->name) == "Guid") continue;  // internal
    out += "panel ch { class: comp_header;\n";
    out += F("  text ct { class: comp_title; text: \"%s\"; }\n", comp->name);
    out += "  panel csp { flex-grow: 1; }\n";
    if (std::string(comp->name) != "Transform" && std::string(comp->name) != "Name")
      out += F("  button insp_rm_%u { text: \"x\"; font-size: 13; color: #7a7d83; cursor: pointer; :hover { color:#e0655f; } }\n",
               comp->id);
    out += "}\n";
    out += "panel cb { layout: column; padding: 8 12; gap: 8;\n";
    for (u32 pi = 0; pi < comp->prop_count; ++pi) {
      const edit::PropDesc& pd = comp->props[pi];
      edit::PropValue v;
      edit::GetProp(*world_, e, *comp, pd, &v);
      out += F("  panel r%u { layout: row; align: center; gap: 8;\n", pi);
      out += F("    text rl { class: row_label; text: \"%s\"; }\n", DisplayName(pd.name).c_str());
      int count = (v.type == edit::PropType::kVec3) ? 3
                  : (v.type == edit::PropType::kVec4 || v.type == edit::PropType::kQuat) ? 3
                  : (v.type == edit::PropType::kVec2) ? 2
                  : 1;
      if (v.type == edit::PropType::kString) {
        out += F("    panel fv { class: field; text vv { class: field_val; text: \"%s\"; } }\n",
                 v.s.c_str());
      } else if (v.type == edit::PropType::kBool) {
        out += F("    button field_%u_%u_0 { width: 18; height: 18; corner-radius: 4; "
                 "background: %s; cursor: pointer; }\n",
                 comp->id, pi, v.b ? "#3a5bbf" : "#33363c");
      } else if (v.type == edit::PropType::kU64 || v.type == edit::PropType::kAssetId ||
                 v.type == edit::PropType::kEntity) {
        std::string txt;
        if (v.type == edit::PropType::kEntity) txt = F("entity %u", v.e.index);
        else txt = F("%016llx", (unsigned long long)v.u);
        out += F("    panel fv { class: field; text vv { class: field_val; color:#8fa9d6; text: \"%s\"; } }\n",
                 txt.c_str());
      } else {
        f32 disp[4] = {v.f[0], v.f[1], v.f[2], v.f[3]};
        if (v.type == edit::PropType::kQuat)
          QuatToEuler(Quat{v.f[0], v.f[1], v.f[2], v.f[3]}, disp);
        out += "    panel fields { layout: row; gap: 5; flex-grow: 1;\n";
        for (int a = 0; a < count; ++a) {
          out += F("      panel field_%u_%u_%d { class: field; cursor: move; "
                   "text fk { font-size: 11; font-weight: bold; color: %s; text: \"%s\"; } "
                   "text fv { class: field_val; text: \"%.3g\"; } }\n",
                   comp->id, pi, a, AxisColor(a), AxisLabel(a), disp[a]);
        }
        out += "    }\n";
      }
      out += "  }\n";
    }
    out += "}\n";
  }

  // Add-component affordance.
  out += "panel addc { padding: 12;\n";
  out += "  button insp_addcomp { text: \"+ Add Component\"; layout: row; justify: center; align:center; height: 30; "
         "background: #2c2f34; corner-radius: 6; border-color:#3a3d42; border-width:1; color:#a7aab0; font-size:12; cursor: pointer; :hover { background:#3a3d44; } }\n";
  if (add_menu_open_) {
    out += "  panel addmenu { layout: column; margin: 6 0 0 0; background: #191a1d; corner-radius: 6; border-color:#33363c; border-width:1; padding: 4;\n";
    for (const edit::ComponentDesc* comp : edit::AllComponents()) {
      if (world_->HasRaw(e, comp->id)) continue;
      if (std::string(comp->name) == "Guid") continue;
      if (std::string(comp->name) == "Name") continue;  // ECS std::string unsafe; edited via side table
      out += F("    button insp_add_%u { text: \"%s\"; font-size: 12; color:#c9ccd2; padding: 7 10; corner-radius:4; cursor: pointer; :hover { background:#2c2f34; } }\n",
               comp->id, comp->name);
    }
    out += "  }\n";
  }
  out += "}\n";
  return out;
}

std::string Editor::BuildContent() {
  std::string out;
  size_t idx = 0;
  for (const AssetEntry& a : assets_list_) {
    if (!content_filter_.empty() && a.name.find(content_filter_) == std::string::npos) { ++idx; continue; }
    out += F("panel card_%zu { layout: column; align: center; gap: 6; width: 96;\n", idx);
    out += F("  button asset_%zu { width: 96; height: 80; corner-radius: 8; layout: row; justify: center; align: center; "
             "background: #191a1d; border-color: %s; border-width: 1; cursor: pointer; :hover { background: #22242a; }\n"
             "    text bt { text: \"%s\"; font-size: 11; font-weight: bold; color: %s; } }\n",
             idx, KindColor(a.kind).c_str(), a.kind.c_str(), KindColor(a.kind).c_str());
    out += F("  text cn { text: \"%s\"; font-size: 11; color: #b7bac0; }\n", a.name.c_str());
    out += "}\n";
    ++idx;
  }
  if (out.empty()) out = "text ce { text: \"(no assets)\"; font-size: 12; color:#5a5d63; padding: 8; }\n";
  return out;
}

std::string Editor::BuildDialog() {
  if (!dialog_open_) return "";
  std::string out = "panel dialog_scrim { position: absolute; left: 0; top: 0; width: 100vw; height: 100vh; "
                    "layout: column; justify: center; align: center; background: #000000aa;\n";
  out += "  panel dialog { layout: column; width: 460; background: #26282c; corner-radius: 10; "
         "border-color:#3a3d42; border-width:1; padding: 0;\n";
  out += "    panel dh { class: panel_header; text dt { class: header_label; text: \"OPEN SCENE\"; } }\n";
  out += "    panel dl { layout: column; padding: 10; gap: 3; max-height: 360; overflow: scroll;\n";
  if (dialog_files_.empty())
    out += "      text de { text: \"No .rxscene files found\"; font-size: 12; color:#6a6d73; padding: 8; }\n";
  for (size_t i = 0; i < dialog_files_.size(); ++i) {
    out += F("      button dlg_%zu { text: \"%s\"; font-size: 13; color:#c9ccd2; padding: 8 10; corner-radius: 5; "
             "cursor: pointer; :hover { background:#2f4368; color:#fff; } }\n",
             i, dialog_files_[i].c_str());
  }
  out += "    }\n";
  out += "    panel df { layout: row; justify: end; padding: 10; gap: 8; border-color:#101113; border-width:1 0 0 0;\n"
         "      button dlg_cancel { text: \"Cancel\"; font-size: 12; color:#c9ccd2; padding: 8 16; background:#2c2f34; corner-radius:6; cursor: pointer; :hover { background:#3a3d44; } } }\n";
  out += "  }\n}\n";
  return out;
}

void Editor::UiHotReloadCheck(f32 dt) {
  reload_timer_ += dt;
  if (reload_timer_ < 0.3f) return;
  reload_timer_ = 0;
  std::string path = (ui_dir_.empty() ? std::string("apps/editor/ui") : ui_dir_) + "/editor.ugui";
  std::error_code ec;
  auto t = fs::last_write_time(path, ec);
  if (ec) return;
  int64_t m = (int64_t)t.time_since_epoch().count();
  if (m != ui_mtime_ && ui_mtime_ != 0) MarkDirty();
}

// ===========================================================================
// Event routing
// ===========================================================================
namespace {
// Climb up to `max` ancestors, returning the first non-empty widget name.
std::string NamedAncestor(ugui::UIContext& ui, ugui::wid w, int max = 5) {
  for (int i = 0; i < max && w.valid(); ++i) {
    if (ugui::WidgetNode* n = ui.world().Get<ugui::WidgetNode>(w)) {
      if (!n->name.empty()) return std::string(n->name.c_str());
    }
    ugui::Hierarchy* h = ui.world().Get<ugui::Hierarchy>(w);
    w = h ? h->parent : ugui::wid{};
  }
  return "";
}
}  // namespace

void Editor::OnUiClick(ugui::wid w, ugui::MouseButton btn) {
  std::string name = NamedAncestor(ui_, w);
  if (name.empty()) return;
  RouteClick(name, btn);
}

bool Editor::RouteClick(const std::string& name, ugui::MouseButton) {
  auto starts = [&](const char* p) { return name.rfind(p, 0) == 0; };
  auto tail_u = [&](const char* p) -> unsigned {
    return (unsigned)std::strtoul(name.c_str() + std::strlen(p), nullptr, 10);
  };

  if (name == "menu_new") { NewScene(); return true; }
  if (name == "menu_open") { OpenFileDialog(); return true; }
  if (name == "menu_save") { if (scene_path_ == "untitled.rxscene") OpenFileDialog(); else DoSave(scene_path_); return true; }
  if (name == "menu_saveas") { DoSave("scene_saved.rxscene"); return true; }
  if (name == "btn_undo") { undo_.Undo(*world_); doc_dirty_ = true; MarkDirty(); return true; }
  if (name == "btn_redo") { undo_.Redo(*world_); doc_dirty_ = true; MarkDirty(); return true; }
  if (name == "btn_play") { playing_ = true; return true; }
  if (name == "btn_stop") { playing_ = false; return true; }
  if (name == "tool_translate") { gizmo_mode_ = GizmoMode::kTranslate; MarkDirty(); return true; }
  if (name == "tool_rotate") { gizmo_mode_ = GizmoMode::kRotate; MarkDirty(); return true; }
  if (name == "tool_scale") { gizmo_mode_ = GizmoMode::kScale; MarkDirty(); return true; }
  if (name == "tab_inspector") { material_tab_ = false; MarkDirty(); return true; }
  if (name == "tab_material") { material_tab_ = true; MarkDirty(); return true; }

  if (name == "hier_add") {
    ecs::Entity out;
    const edit::ComponentDesc* xf = edit::FindComponentByName("Transform");
    const edit::ComponentDesc* nm = edit::FindComponentByName("Name");
    std::vector<std::pair<const edit::ComponentDesc*,
                          std::vector<std::pair<const edit::PropDesc*, edit::PropValue>>>>
        initial;
    initial.push_back({xf, {}});
    initial.push_back({nm, {{&nm->props[0], edit::PropValue::String("Empty")}}});
    undo_.Push(*world_, edit::MakeCreateEntity(std::move(initial), &out));
    selection_.Set(out);
    doc_dirty_ = true;
    MarkDirty();
    return true;
  }
  if (name == "hier_del") {
    if (ecs::Entity e = selection_.primary()) {
      undo_.Push(*world_, edit::MakeDestroyEntity(*world_, e));
      selection_.Clear();
      doc_dirty_ = true;
      MarkDirty();
    }
    return true;
  }
  if (starts("hier_")) {
    // hier_<index>_<gen>
    unsigned index = 0, gen = 0;
    if (std::sscanf(name.c_str() + 5, "%u_%u", &index, &gen) == 2) {
      ecs::Entity e{index, gen};
      if (world_->IsAlive(e)) { selection_.Set(e); MarkDirty(); }
    }
    return true;
  }
  if (starts("insp_addcomp")) { add_menu_open_ = !add_menu_open_; MarkDirty(); return true; }
  if (starts("insp_add_")) {
    ecs::ComponentId id = tail_u("insp_add_");
    if (const edit::ComponentDesc* c = edit::FindComponent(id))
      if (ecs::Entity e = selection_.primary()) {
        undo_.Push(*world_, edit::MakeAddComponent(*world_, e, *c));
        add_menu_open_ = false;
        doc_dirty_ = true;
        MarkDirty();
      }
    return true;
  }
  if (starts("insp_rm_")) {
    ecs::ComponentId id = tail_u("insp_rm_");
    if (const edit::ComponentDesc* c = edit::FindComponent(id))
      if (ecs::Entity e = selection_.primary()) {
        undo_.Push(*world_, edit::MakeRemoveComponent(*world_, e, *c));
        doc_dirty_ = true;
        MarkDirty();
      }
    return true;
  }
  // toggle bool field (field_<compid>_<prop>_0 on a bool)
  if (starts("field_") && name.find("_tint_") == std::string::npos) {
    unsigned cid = 0, pi = 0, ax = 0;
    if (std::sscanf(name.c_str() + 6, "%u_%u_%u", &cid, &pi, &ax) == 3) {
      const edit::ComponentDesc* c = edit::FindComponent(cid);
      ecs::Entity e = selection_.primary();
      if (c && e && pi < c->prop_count && c->props[pi].type == edit::PropType::kBool) {
        edit::PropValue v;
        edit::GetProp(*world_, e, *c, c->props[pi], &v);
        undo_.Push(*world_, edit::MakeSetProp(*world_, e, *c, c->props[pi],
                                              edit::PropValue::Bool(!v.b)));
        doc_dirty_ = true;
        MarkDirty();
      }
    }
    return true;
  }
  if (starts("asset_")) {
    unsigned i = tail_u("asset_");
    if (i < assets_list_.size()) {
      const AssetEntry& a = assets_list_[i];
      if (a.kind == "scene") {
        DoLoad(a.name);
      } else if (a.kind == "mesh") {
        // spawn in front of the camera
        asset::AssetId mesh = a.name == "sphere.mesh" ? sphere_mesh_
                              : a.name == "plane.mesh"  ? plane_mesh_
                                                        : cube_mesh_;
        Vec3 pos = camera_.position() + camera_.forward() * 4.0f;
        const edit::ComponentDesc* xf = edit::FindComponentByName("Transform");
        const edit::ComponentDesc* rd = edit::FindComponentByName("Renderable");
        const edit::ComponentDesc* nm = edit::FindComponentByName("Name");
        std::vector<std::pair<const edit::ComponentDesc*,
                              std::vector<std::pair<const edit::PropDesc*, edit::PropValue>>>>
            initial;
        initial.push_back({xf, {{&xf->props[0], edit::PropValue::Vec3(pos.x, pos.y, pos.z)}}});
        initial.push_back({rd, {{&rd->props[0], edit::PropValue::AssetIdV(mesh.hash)}}});
        initial.push_back({nm, {{&nm->props[0], edit::PropValue::String(a.name)}}});
        ecs::Entity out;
        undo_.Push(*world_, edit::MakeCreateEntity(std::move(initial), &out));
        selection_.Set(out);
        doc_dirty_ = true;
        MarkDirty();
      }
    }
    return true;
  }
  if (starts("dlg_cancel")) { dialog_open_ = false; MarkDirty(); return true; }
  if (starts("dlg_")) {
    unsigned i = tail_u("dlg_");
    if (i < dialog_files_.size()) { DoLoad(dialog_files_[i]); dialog_open_ = false; }
    return true;
  }
  return false;
}

void Editor::OnUiTextSubmit(const std::string& widget, const std::string& value) {
  if (widget == "insp_name") {
    if (ecs::Entity e = selection_.primary()) {
      SetName(e, value);  // side table (ECS std::string is unsafe today)
      doc_dirty_ = true;
      MarkDirty();
    }
  }
}

// ===========================================================================
// OnBuildView: gather draws + drive the UI production
// ===========================================================================
void Editor::OnBuildView(f32 dt, render::FrameView& view) {
  view.camera.eye = camera_.position();
  view.camera.target = camera_.target();
  view.frame_delta_seconds = dt > 0 ? dt : 1.0f / 60.0f;

  // Manual gather so we can apply parent transforms, per-entity/selection tint
  // and the pick ids the GPU pick pass writes.
  ecs::Entity primary = selection_.primary();
  pick_map_.clear();
  u32 next_pick_id = 1;  // 0 = background/unpickable
  world_->Each<scene::Transform, scene::Renderable>(
      [&](ecs::Entity e, scene::Transform&, scene::Renderable& r) {
        if (world_->Has<scene::Hidden>(e)) return;
        render::DrawItem d;
        d.mesh = r.mesh.hash;
        d.transform = edit::WorldMatrix(*world_, e);
        d.prev_transform = d.transform;
        uint32_t tint = tints_.count(e.index) ? tints_[e.index] : 0u;
        if (e == primary) tint = 0xffa64d;  // selection highlight
        d.tint = tint;
        d.pick_id = next_pick_id;
        pick_map_[next_pick_id] = e;
        ++next_pick_id;
        view.draws.push_back(d);
      });

  // 1m ground grid through the depth-tested debug-line pass.
  if (grid_lines_.empty()) {
    const int n = 10;
    for (int i = -n; i <= n; ++i) {
      u32 c = i == 0 ? 0x5a5d63ff : 0x33363caa;
      grid_lines_.push_back({{(f32)i, 0.011f, (f32)-n}, {(f32)i, 0.011f, (f32)n}, c});
      grid_lines_.push_back({{(f32)-n, 0.011f, (f32)i}, {(f32)n, 0.011f, (f32)i}, c});
    }
  }
  view.debug_lines = grid_lines_;

  // Translate-gizmo axis lines, drawn on top of the scene.
  gizmo_lines_.clear();
  if (primary && world_->IsAlive(primary) && gizmo_mode_ == GizmoMode::kTranslate) {
    scene::Transform wt = edit::WorldTransform(*world_, primary);
    Vec3 o{wt.position[0], wt.position[1], wt.position[2]};
    f32 len = std::max(0.5f, Length(o - camera_.position()) * 0.18f);
    gizmo_lines_.push_back({o, {o.x + len, o.y, o.z}, 0xe0655fff});  // X red
    gizmo_lines_.push_back({o, {o.x, o.y + len, o.z}, 0x7fb96aff});  // Y green
    gizmo_lines_.push_back({o, {o.x, o.y, o.z + len}, 0x5a8deeff});  // Z blue
  }
  view.debug_lines_overlay = gizmo_lines_;

  if (!ui_ready_) return;
  if (ui_dirty_) UiRebuild();
  UiPerFrameText();
  UpdateGizmoWidgets();

  draw_data_ = &ui_.RenderDrawData();
  view.needs_blur = false;
  for (u32 i = 0; i < draw_data_->command_count; ++i)
    if (draw_data_->commands[i].blur > 0) { view.needs_blur = true; break; }

  if (ui_.text_engine().atlas_revision() != font_revision_) {
    ugui::Vec2 as = ui_.text_engine().atlas_size();
    backend_.UpdateFontAtlas(ui_.text_engine().atlas_pixels(), (u32)as.x, (u32)as.y);
    font_revision_ = ui_.text_engine().atlas_revision();
  }
  backend_.NewFrame();
  view.hud_draw = [this, &view](render::CommandList& cmd) {
    backend_.SetBackdrop(render::GetVkImageView(view.blur_source),
                         render::GetVkSampler(view.blur_sampler));
    if (draw_data_) backend_.Render(*draw_data_, render::GetVkCommandBuffer(cmd));
  };
}

// scrub helpers implemented here (need ugui hovered name + reflect)
bool Editor::TryStartScrub(f32 mx) {
  std::string name = NamedAncestor(ui_, ui_.input().hovered_widget());
  if (name.rfind("field_", 0) != 0) return false;
  unsigned cid = 0, pi = 0, ax = 0;
  if (name.find("_tint_") != std::string::npos) {
    if (std::sscanf(name.c_str() + std::strlen("field_tint_"), "%u_%u", &pi, &ax) == 2) {
      // tint scrub: live edit of tints_ (not undo-tracked)
      scrub_ = {};
      scrub_.active = true;
      scrub_.entity = selection_.primary();
      scrub_.comp = nullptr;  // signals tint
      scrub_.axis = (int)ax;
      scrub_.start_mouse = mx;
      uint32_t t = tints_.count(scrub_.entity.index) ? tints_[scrub_.entity.index] : 0xffffff;
      scrub_.base_value = (f32)((t >> (16 - ax * 8)) & 0xFF);
      scrub_.step = 1.0f;
      return true;
    }
    return false;
  }
  if (std::sscanf(name.c_str() + 6, "%u_%u_%u", &cid, &pi, &ax) != 3) return false;
  const edit::ComponentDesc* c = edit::FindComponent(cid);
  ecs::Entity e = selection_.primary();
  if (!c || !e || pi >= c->prop_count) return false;
  const edit::PropDesc& pd = c->props[pi];
  if (pd.type == edit::PropType::kBool || pd.type == edit::PropType::kString) return false;
  edit::PropValue v;
  edit::GetProp(*world_, e, *c, pd, &v);
  scrub_ = {};
  scrub_.active = true;
  scrub_.entity = e;
  scrub_.comp = c;
  scrub_.prop = &pd;
  scrub_.axis = (int)ax;
  scrub_.start_mouse = mx;
  scrub_.step = (pd.type == edit::PropType::kQuat) ? 0.5f : 0.02f;
  if (pd.type == edit::PropType::kQuat) {
    f32 euler[3];
    QuatToEuler(Quat{v.f[0], v.f[1], v.f[2], v.f[3]}, euler);
    scrub_.base_value = euler[ax];
    scrub_.base_euler[0] = euler[0]; scrub_.base_euler[1] = euler[1]; scrub_.base_euler[2] = euler[2];
  } else {
    scrub_.base_value = v.f[ax];
  }
  undo_.BeginGroup("Edit");
  return true;
}

void Editor::UpdateScrub() {
  if (!scrub_.active) return;
  const InputState& in = window_->input();
  f32 dx = in.mouse_x - scrub_.start_mouse;
  f32 nv = scrub_.base_value + dx * scrub_.step;

  if (!scrub_.comp) {  // tint live edit
    ecs::Entity e = scrub_.entity;
    int iv = (int)std::clamp(nv, 0.0f, 255.0f);
    uint32_t t = tints_.count(e.index) ? tints_[e.index] : 0xffffff;
    int shift = 16 - scrub_.axis * 8;
    t = (t & ~(0xFFu << shift)) | ((uint32_t)iv << shift);
    tints_[e.index] = t;
    MarkDirty();
    return;
  }

  const edit::PropDesc& pd = *scrub_.prop;
  edit::PropValue v;
  edit::GetProp(*world_, scrub_.entity, *scrub_.comp, pd, &v);
  if (pd.type == edit::PropType::kQuat) {
    f32 euler[3] = {scrub_.base_euler[0], scrub_.base_euler[1], scrub_.base_euler[2]};
    euler[scrub_.axis] = scrub_.base_value + dx * scrub_.step;
    Quat q = EulerToQuat(euler);
    undo_.Push(*world_, edit::MakeSetProp(*world_, scrub_.entity, *scrub_.comp, pd,
                                          edit::PropValue::Quat(q.x, q.y, q.z, q.w)));
  } else {
    if (pd.min != pd.max) nv = std::clamp(nv, pd.min, pd.max);
    edit::PropValue out = v;
    out.f[scrub_.axis] = nv;
    undo_.Push(*world_, edit::MakeSetProp(*world_, scrub_.entity, *scrub_.comp, pd, out));
  }
  doc_dirty_ = true;
  MarkDirty();
}

}  // namespace rx::editor
