// The editor's ugui layer: draw-data-mode UIContext driven by the recreation-
// style GuiRenderBackend through FrameView::hud_draw, plus the C++ that
// generates the .ugui document from editor state and routes widget events.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ugui/core/color.h>
#include <ugui/style/style.h>
#include <ugui/widgets/button.h>
#include <ugui/widgets/text.h>
#include <ugui/widgets/text_input.h>
#include <ugui/widgets/widget.h>
#include <ugui/widgets/widget_registry.h>

#include "editor_app.h"
#include "foundation/files/file_system.h"
#include "foundation/logging/log.h"
#include "rxe/scene/hierarchy.h"

#include "base/algorithm.h"
#include "base/containers/pair.h"
#include "base/containers/vector.h"
#include "base/functional/function.h"
#include "base/memory/move.h"
#include "base/strings/string_ref.h"
#include "base/strings/xstring.h"
#include "foundation/math/scalar.h"
#include "rxe/anim/morph.h"
#include "rxe/gpu/rhi/vulkan_interop.h"
#include "rxe/scene/components.h"

namespace rx::editor {
namespace {
const char *FindFont() {
  static base::String resolved;
  if (FILE *p = popen("fc-match -f '%{file}' sans 2>/dev/null", "r")) {
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, p);
    pclose(p);
    if (n > 0) {
      buf[n] = '\0';
      resolved = buf;
      if (fs::Exists(resolved))
        return resolved.c_str();
    }
  }
  static const char *candidates[] = {
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/TTF/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
      "/run/current-system/sw/share/X11/fonts/DejaVuSans.ttf",
  };
  for (auto *c : candidates)
    if (fs::Exists(c))
      return c;
  return nullptr;
}

base::String F(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list measure;
  va_copy(measure, ap);
  const int size = vsnprintf(nullptr, 0, fmt, measure);
  va_end(measure);
  if (size < 0) {
    va_end(ap);
    return {};
  }
  base::String result(static_cast<size_t>(size), '\0');
  vsnprintf(result.data(), result.size() + 1, fmt, ap);
  va_end(ap);
  return result;
}

base::String EscapeUguiString(base::StringRef value) {
  base::String escaped;
  escaped.reserve(value.size());
  for (char c : value) {
    switch (c) {
    case '\\':
      escaped += "\\\\";
      break;
    case '"':
      escaped += "\\\"";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      escaped += static_cast<unsigned char>(c) < 0x20 ? '?' : c;
      break;
    }
  }
  return escaped;
}

// Quaternion -> euler XYZ degrees (for the rotation row).
void QuatToEuler(const Quat &q, f32 out_deg[3]) {
  f32 sinr = 2 * (q.w * q.x + q.y * q.z);
  f32 cosr = 1 - 2 * (q.x * q.x + q.y * q.y);
  f32 x = ::atan2f(sinr, cosr);
  f32 sinp = 2 * (q.w * q.y - q.z * q.x);
  f32 y =
      ::fabsf(sinp) >= 1 ? ::copysignf(1.5707963f, sinp) : ::asinf(sinp);
  f32 siny = 2 * (q.w * q.z + q.x * q.y);
  f32 cosy = 1 - 2 * (q.y * q.y + q.z * q.z);
  f32 z = ::atan2f(siny, cosy);
  const f32 r2d = 57.29578f;
  out_deg[0] = x * r2d;
  out_deg[1] = y * r2d;
  out_deg[2] = z * r2d;
}
Quat EulerToQuat(const f32 deg[3]) {
  const f32 d2r = 0.0174533f;
  f32 cx = ::cosf(deg[0] * d2r * 0.5f), sx = ::sinf(deg[0] * d2r * 0.5f);
  f32 cy = ::cosf(deg[1] * d2r * 0.5f), sy = ::sinf(deg[1] * d2r * 0.5f);
  f32 cz = ::cosf(deg[2] * d2r * 0.5f), sz = ::sinf(deg[2] * d2r * 0.5f);
  return Normalize(
      Quat{sx * cy * cz - cx * sy * sz, cx * sy * cz + sx * cy * sz,
           cx * cy * sz - sx * sy * cz, cx * cy * cz + sx * sy * sz});
}

// Engine reflection registers lowercase prop names ("position"); display them
// capitalized in the inspector.
base::String DisplayName(const char *n) {
  base::String s = n ? n : "";
  if (!s.empty() && s[0] >= 'a' && s[0] <= 'z')
    s[0] = (char)(s[0] - 'a' + 'A');
  return s;
}

const char *AxisLabel(int i) {
  return i == 0 ? "X" : i == 1 ? "Y" : i == 2 ? "Z" : "W";
}
const char *AxisColor(int i) {
  return i == 0   ? "#e0655f"
         : i == 1 ? "#7fb96a"
         : i == 2 ? "#5a8dee"
                  : "#c9a25a";
}
base::String KindColor(const base::String &k) {
  if (k == "mesh")
    return "#2f7f6a";
  if (k == "terrain")
    return "#6f8f4e";
  if (k == "texture")
    return "#9a7f3a";
  if (k == "material")
    return "#6a4fbf";
  if (k == "scene")
    return "#bf5a7f";
  if (k == "audio")
    return "#5a7fbf";
  return "#4a4d52";
}
} // namespace

// ugui init / shutdown
bool Editor::UiInit() {
  host_state_.window_width = (f32)window_->width();
  host_state_.window_height = (f32)window_->height();

  ugui::UIConfig cfg;
  cfg.draw_data = true;
  cfg.external_window = &host_state_;
  cfg.width = window_->width();
  cfg.height = window_->height();
  ui_.set_ui_scale(window_->pixel_density());  // before Init: it scales too
  if (!ui_.Init(cfg))
    return false;

  if (const char *font = FindFont())
    ui_.set_default_font(font_ = ui_.LoadFont(font));
  else
    RX_WARN("editor: no system font found; text will not render");

  gpu::VulkanHandles vk = gpu::GetVulkanHandles(*renderer_->device());
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
  bi.color_format = gpu::GetVkFormat(renderer_->swapchain_format());
  bi.frames_in_flight = gpu::GetVulkanFramesInFlight(*renderer_->device());
  if (!backend_.Init(bi))
    return false;
  ui_.set_texture_backend(&backend_);

  ui_.input().set_on_click(
      [this](ugui::wid w, ugui::MouseButton b) { OnUiClick(w, b); });

  // Locate the .ugui source dir (compile-def default, env override).
#ifdef RX_EDITOR_UI_DIR
  ui_dir_ = RX_EDITOR_UI_DIR;
#endif
  if (const char *env = ::getenv("RX_EDITOR_UI_DIR"))
    ui_dir_ = env;

  ui_ready_ = true;
  UiRebuild();
  return true;
}

void Editor::UiShutdown() {
  backend_.Shutdown();
  ui_.Shutdown();
  ui_ready_ = false;
}

// Input feeding
void Editor::UiFeedInput(f32) {
  const InputState &in = window_->input();
  host_state_.window_width = (f32)window_->width();
  host_state_.window_height = (f32)window_->height();
  // The canvas above is pixels, so every px size in the .ugui documents has to
  // grow by the same factor or the whole UI shrinks to the ratio between the
  // buffer and the size the desktop lays the window out at. Re-set each frame
  // because dragging the window to a differently scaled monitor changes it.
  ui_.set_ui_scale(window_->pixel_density());

  ugui::InputQueue &q = ui_.platform()->input_queue();
  q.PushMove({in.mouse_x, in.mouse_y});

  const ugui::MouseButton ub[3] = {ugui::MouseButton::kLeft,
                                   ugui::MouseButton::kRight,
                                   ugui::MouseButton::kMiddle};
  const MouseButton rb[3] = {MouseButton::kLeft, MouseButton::kRight,
                             MouseButton::kMiddle};
  static bool prev[3] = {false, false, false};
  for (int i = 0; i < 3; ++i) {
    bool down = in.button(rb[i]);
    if (down != prev[i])
      q.PushButton(ub[i], down);
    prev[i] = down;
  }
  if (in.wheel != 0)
    q.PushScroll({0, in.wheel});

  // Text + editing keys (GLFW codes, as the ugui text-input expects).
  int shift = in.key(Key::kLeftShift) ? 0x0001 : 0;
  auto key_edge = [&](Key k) { return in.key_pressed(k); };
  if (key_edge(Key::kBackspace))
    q.PushKey(259, 0, true, false, shift);
  if (key_edge(Key::kReturn))
    q.PushKey(257, 0, true, false, shift);
  if (key_edge(Key::kTab))
    q.PushKey(258, 0, true, false, shift);
  if (key_edge(Key::kDelete))
    q.PushKey(261, 0, true, false, shift);
  if (key_edge(Key::kArrowLeft))
    q.PushKey(263, 0, true, false, shift);
  if (key_edge(Key::kArrowRight))
    q.PushKey(262, 0, true, false, shift);
  // UTF-8 text this pump -> codepoints.
  for (u8 i = 0; i < in.text_len;) {
    unsigned char c = (unsigned char)in.text[i];
    u32 cp = 0, n = 1;
    if (c < 0x80) {
      cp = c;
      n = 1;
    } else if ((c >> 5) == 0x6) {
      cp = c & 0x1F;
      n = 2;
    } else if ((c >> 4) == 0xE) {
      cp = c & 0x0F;
      n = 3;
    } else if ((c >> 3) == 0x1E) {
      cp = c & 0x07;
      n = 4;
    }
    for (u32 k = 1; k < n && i + k < in.text_len; ++k)
      cp = (cp << 6) | (in.text[i + k] & 0x3F);
    if (cp >= 32)
      q.PushChar(cp);
    i += n;
  }
}

// Per-frame text + gizmo widget updates
void Editor::UiPerFrameText() {
  auto set = [&](const char *name, const base::String &v) {
    ugui::wid w = ui_.FindWidget(name);
    if (w.valid())
      ugui::SetText(w, v);
  };
  set("status_fps", F("%.0f FPS", fps_));
  set("status_path", scene_path_ + "  |  " + terrain_path_);
  set("status_dirty", doc_dirty_ && terrain_dirty_
                          ? "* scene + terrain modified"
                      : doc_dirty_     ? "* scene modified"
                      : terrain_dirty_ ? "* terrain modified"
                                       : "");
  set("status_message", status_message_);
  if (imported_models_.empty()) {
    set("vp_walk", "");
  } else {
    const char *active =
        anim::WalkStyleName(imported_models_.front().active_walk_style);
    set("vp_walk", walk_preview_mode_ == WalkPreviewMode::kAuto
                       ? F("Walk: Auto (%s)", active)
                       : F("Walk: %s", active));
  }
  const size_t scene_entities =
      world_->entity_count() >= terrain_tiles_.size()
          ? world_->entity_count() - terrain_tiles_.size()
          : 0;
  set("status_count", F("%zu scene / %zu terrain tiles", scene_entities,
                        terrain_tiles_.size()));
  set("btn_undo",
      undo_.can_undo() ? F("Undo: %s", undo_.undo_label()) : "Undo");
  set("btn_redo",
      undo_.can_redo() ? F("Redo: %s", undo_.redo_label()) : "Redo");
  set("vp_gizmo", editor_mode_ == EditorMode::kTerrain   ? "Tool: Terrain"
                  : editor_mode_ == EditorMode::kPlace   ? "Tool: Place"
                  : gizmo_mode_ == GizmoMode::kTranslate ? "Gizmo: Move"
                  : gizmo_mode_ == GizmoMode::kRotate    ? "Gizmo: Rotate"
                                                         : "Gizmo: Scale");
}

void Editor::UpdateGizmoWidgets() {
  ecs::Entity e = selection_.primary();
  bool show = e && editor_mode_ == EditorMode::kSelect &&
              gizmo_mode_ == GizmoMode::kTranslate;
  scene::Transform wt =
      e ? scene::WorldTransform(*world_, e) : scene::Transform{};
  Vec3 origin{wt.position[0], wt.position[1], wt.position[2]};
  f32 len = e ? rx::Max(0.5f, Length(origin - camera_.position()) * 0.18f) : 0;
  for (int a = 0; a < 3; ++a) {
    ugui::wid w = ui_.FindWidget(F("gizmo_%c", "xyz"[a]).c_str());
    if (!w.valid())
      continue;
    ugui::StyleC *sc = ui_.world().Get<ugui::StyleC>(w);
    if (!sc)
      continue;
    ugui::Style s = sc->style;
    if (!show) {
      s.visibility = ugui::Visibility::kHidden;
    } else {
      Vec3 axis{a == 0 ? 1.0f : 0.0f, a == 1 ? 1.0f : 0.0f,
                a == 2 ? 1.0f : 0.0f};
      bool infront;
      Vec2 tip = ProjectToScreen(origin + axis * len, &infront);
      s.visibility =
          infront ? ugui::Visibility::kVisible : ugui::Visibility::kHidden;
      s.position = ugui::Position::kAbsolute;
      s.left_offset = ugui::Length::Px(tip.x - 7.0f);
      s.top = ugui::Length::Px(tip.y - 7.0f);
    }
    ugui::SetStyle(ui_.world(), w, s);
  }
}

// Document generation
void Editor::UiRebuild() {
  if (!ui_ready_)
    return;
  base::String doc = UiBuildDoc();
  ui_.LoadUiString(doc.c_str(), "editor");
  ui_.InvalidateWidgetCache();

  // Wire text-input handlers (they survive until the next rebuild).
  if (ugui::wid s = ui_.FindWidget("hier_search"); s.valid()) {
    ugui::SetTextInputValue(s, search_filter_);
    ugui::SetTextInputChange(s, [this](const base::String &v) {
      search_filter_ = v;
      MarkDirty();
    });
  }
  if (ugui::wid c = ui_.FindWidget("content_filter"); c.valid()) {
    ugui::SetTextInputValue(c, content_filter_);
    ugui::SetTextInputChange(c, [this](const base::String &v) {
      content_filter_ = v;
      MarkDirty();
    });
  }
  if (ugui::wid n = ui_.FindWidget("insp_name"); n.valid()) {
    if (ecs::Entity e = selection_.primary(); e && world_->IsAlive(e))
      ugui::SetTextInputValue(n, EntityLabel(e));
    ugui::SetTextInputSubmit(
        n, [this](const base::String &v) { OnUiTextSubmit("insp_name", v); });
  }
  ui_.input().RefreshHover(ui_.root());
  UiPerFrameText();
  ui_dirty_ = false;
}

base::String Editor::UiBuildDoc() {
  base::String tmpl;
  base::String path =
      (ui_dir_.empty() ? base::String("apps/editor/ui") : ui_dir_) +
      "/editor.ugui";
  if (FILE *f = ::fopen(path.c_str(), "rb")) {
    char buf[4096];
    size_t n;
    while ((n = ::fread(buf, 1, sizeof(buf), f)) > 0)
      tmpl.append(buf, n);
    ::fclose(f);
    ui_mtime_ = fs::LastWriteTime(path).value_or(0);
  } else {
    RX_WARN("editor: cannot read {}", path);
    return "panel root { background: #101113; text t { text: \"missing "
           "editor.ugui\"; color:#f00; } }";
  }
  auto replace = [&](const char *marker, const base::String &with) {
    size_t p = tmpl.find(marker);
    if (p != base::String::npos)
      tmpl = tmpl.substr(0, p) + with + tmpl.substr(p + ::strlen(marker));
  };
  replace("//@@HIERARCHY@@", BuildHierarchy());
  replace("//@@MODES@@", BuildModeToolbar());
  replace("//@@INSPECTOR_TABS@@", BuildInspectorTabs());
  replace("//@@INSPECTOR@@", BuildInspector());
  replace("//@@CONTENT@@", BuildContent());
  replace(
      "//@@GIZMO@@",
      "panel gizmo_x { position: absolute; width: 14; height: 14; background: "
      "#e0655f; corner-radius: 7; border-color:#ffffff; border-width:1; }\n"
      "panel gizmo_y { position: absolute; width: 14; height: 14; background: "
      "#7fb96a; corner-radius: 7; border-color:#ffffff; border-width:1; }\n"
      "panel gizmo_z { position: absolute; width: 14; height: 14; background: "
      "#5a8dee; corner-radius: 7; border-color:#ffffff; border-width:1; }\n");
  replace("//@@DIALOG@@", BuildDialog());
  return tmpl;
}

base::String Editor::BuildHierarchy() {
  base::String out;
  // Recursive emit respecting Parent; filter by search.
  base::Function<void(ecs::Entity, int)> emit = [&](ecs::Entity e, int depth) {
    if (IsTerrainVisual(e))
      return;
    base::String label = EntityLabel(e);
    bool match = search_filter_.empty() ||
                 label.find(search_filter_) != base::String::npos;
    if (match) {
      const base::String escaped_label = EscapeUguiString(label);
      bool sel = selection_.Contains(e);
      const char *icon =
          world_->Has<scene::Renderable>(e) ? "#e0b06a" : "#c78ad6";
      out += F(
          "button hier_%u_%u { layout: row; align: center; height: 26; gap: 8; "
          "padding: 0 6 0 %d; corner-radius: 5; cursor: pointer; %s "
          ":hover { background: #2a2d33; }\n",
          e.index, e.generation, 8 + depth * 16,
          sel ? "background: #2f4368; border-color: #5a8dee; border-width: 0 0 "
                "0 2;"
              : "");
      out += F("  panel d { width: 8; height: 8; corner-radius: 4; background: "
               "%s; }\n",
               icon);
      out += F("  text l { text: \"%s\"; font-size: 13; color: %s; }\n",
               escaped_label.c_str(), sel ? "#ffffff" : "#c9ccd2");
      out += "}\n";
    }
    // children
    world_->Each<scene::Parent>([&](ecs::Entity c, scene::Parent &p) {
      if (p.value == e && !IsTerrainVisual(c))
        emit(c, depth + 1);
    });
  };
  world_->Each<scene::Transform>([&](ecs::Entity e, scene::Transform &) {
    if (!world_->Has<scene::Parent>(e) && !IsTerrainVisual(e))
      emit(e, 0);
  });
  if (out.empty())
    out = "text empty { text: \"(no entities)\"; font-size: 12; color: "
          "#5a5d63; padding: 8; }\n";
  return out;
}

base::String Editor::BuildInspector() {
  if (editor_mode_ == EditorMode::kTerrain)
    return BuildTerrainInspector();
  if (editor_mode_ == EditorMode::kPlace)
    return BuildPlacementInspector();
  ecs::Entity e = selection_.primary();
  if (!e || !world_->IsAlive(e))
    return "text nosel { text: \"No selection\"; font-size: 13; color: "
           "#6a6d73; padding: 16; }\n";

  base::String out;
  const base::String escaped_entity_label = EscapeUguiString(EntityLabel(e));
  out += "panel name_row { layout: row; align: center; padding: 10 12; gap: 8; "
         "border-color:#101113; border-width:0 0 1 0;\n";
  out += "  panel ni { width: 8; height: 8; corner-radius: 4; background: "
         "#e0b06a; }\n";
  out += F(
      "  text-input insp_name { value: \"%s\"; font-size: 13; color: #eceef2; "
      "background: #191a1d; corner-radius: 6; border-color: #33363c; "
      "border-width: 1; "
      "padding: 6 10; flex-grow: 1; tab-index: 5; }\n",
      escaped_entity_label.c_str());
  out += "}\n";

  if (material_tab_) {
    // Material tab: submesh materials + a live tint override.
    out += "panel mat_head { class: comp_header; text mt { class: comp_title; "
           "text: \"Material\"; } }\n";
    scene::Renderable *r = world_->Get<scene::Renderable>(e);
    const MeshRecord *rec = r ? FindMesh(r->mesh.hash) : nullptr;
    int subs = 0;
    if (rec && !rec->mesh.lods.empty())
      subs = (int)rec->mesh.lods[0].submeshes.size();
    const base::String escaped_mesh_name =
        EscapeUguiString(rec ? rec->name : "(none)");
    out += F("panel matb { layout: column; padding: 10 12; gap: 8;\n"
             "  panel mrow { layout: row; gap: 8; align:center; text ml { "
             "class: row_label; text: \"Mesh\"; } "
             "panel mv { class: field; text mvv { class: field_val; "
             "color:#8fa9d6; text: \"%s\"; } } }\n"
             "  panel srow { layout: row; gap: 8; align:center; text sl { "
             "class: row_label; text: \"Submeshes\"; } "
             "panel sv { class: field; text svv { class: field_val; text: "
             "\"%d\"; } } }\n",
             escaped_mesh_name.c_str(), subs);
    uint32_t tint = tints_.contains(e.index) ? tints_[e.index] : 0xffffff;
    out += F("  panel trow { layout: row; gap: 8; align:center; text tl { "
             "class: row_label; text: \"Tint\"; }\n"
             "    panel tsw { width: 26; height: 26; corner-radius: 5; "
             "border-color:#33363c; border-width:1; background: #%06x; }\n",
             tint);
    for (int a = 0; a < 3; ++a) {
      int v = (tint >> (16 - a * 8)) & 0xFF;
      out += F("    panel field_tint_0_%d { class: field; text fk { "
               "font-size:11; font-weight:bold; color:%s; text:\"%s\"; } "
               "text fv { class: field_val; text: \"%d\"; } }\n",
               a, AxisColor(a),
               a == 0   ? "R"
               : a == 1 ? "G"
                        : "B",
               v);
    }
    out += "  }\n}\n";
    out += "  panel matnote { padding: 4 12; text mn { text: \"Tint is a live "
           "per-draw base-color override (DrawItem.tint).\"; font-size: 11; "
           "color:#6a6d73; } }\n";
    return out;
  }

  // What the cook would make of this entity. Baking sorts every object into
  // one of two very different things - an ECS row or a page row with no
  // identity at all - and until this label existed the only way to find out
  // which was to bake and inspect the archive. The cell size is printed because
  // it comes from the bake options rather than the scene, so a label that left
  // it out could disagree with the archive and give no clue why.
  {
    const world::BakeVerdict verdict = world::ClassifyForBake(*world_, e, world_bake_options_);
    const bool refused = verdict.role == world::BakeRole::kRefused;
    out += "panel bakeh { class: comp_header; text bt { class: comp_title; text: \"World "
           "bake\"; } }\n";
    out += "panel bakeb { layout: column; padding: 8 12; gap: 4;\n";
    out += F("  text bakerole { text: \"%s\"; font-size: 12; color: %s; }\n",
             world::BakeRoleName(verdict.role), refused ? "#e0655f" : "#9fd08a");
    if (refused) {
      out += F("  text bakewhy { text: \"%s\"; font-size: 11; color: #a8abb2; }\n",
               EscapeUguiString(verdict.refusal).c_str());
    } else if (verdict.has_cell) {
      const scene::Transform *t = world_->Get<scene::Transform>(e);
      const f32 to_min_x = t->position[0] - verdict.cell_minimum.x;
      const f32 to_max_x = verdict.cell_maximum.x - t->position[0];
      const f32 to_min_z = t->position[2] - verdict.cell_minimum.z;
      const f32 to_max_z = verdict.cell_maximum.z - t->position[2];
      const f32 nearest = rx::Min(rx::Min(to_min_x, to_max_x), rx::Min(to_min_z, to_max_z));
      out += F("  text bakecell { text: \"cell %016llx   [%.0f, %.0f]..[%.0f, %.0f] @ %g m\"; "
               "font-size: 11; color: #a8abb2; }\n",
               static_cast<unsigned long long>(verdict.cell),
               static_cast<double>(verdict.cell_minimum.x),
               static_cast<double>(verdict.cell_minimum.z),
               static_cast<double>(verdict.cell_maximum.x),
               static_cast<double>(verdict.cell_maximum.z),
               static_cast<double>(world_bake_options_.cell_size));
      // An object close to a seam changes cell - and therefore its stable id
      // range - on a small nudge, which is the one thing about the partition an
      // author has to keep in mind while placing things.
      out += F("  text bakeedge { text: \"%.1f m from the nearest cell edge\"; font-size: 11; "
               "color: %s; }\n",
               static_cast<double>(nearest), nearest < 1.0f ? "#e0b06a" : "#6a6d73");
    }
    if (!verdict.dropped.empty()) {
      base::String dropped;
      for (size_t i = 0; i < verdict.dropped.size(); ++i) {
        dropped += (i != 0 ? ", " : "") + verdict.dropped[i];
      }
      out += F("  text bakedrop { text: \"dropped when baked: %s\"; font-size: 11; "
               "color: #e0b06a; }\n",
               EscapeUguiString(dropped).c_str());
    }
    out += "}\n";
  }

  // Inspector tab: one section per component.
  for (const scene::ComponentDesc *comp : scene::ComponentsOn(*world_, e)) {
    if (base::String(comp->name) == "Guid")
      continue; // internal
    out += "panel ch { class: comp_header;\n";
    out += F("  text ct { class: comp_title; text: \"%s\"; }\n", comp->name);
    out += "  panel csp { flex-grow: 1; }\n";
    if (base::String(comp->name) != "Transform" &&
        base::String(comp->name) != "Name")
      out += F("  button insp_rm_%u { text: \"x\"; font-size: 13; color: "
               "#7a7d83; cursor: pointer; :hover { color:#e0655f; } }\n",
               comp->id);
    out += "}\n";
    out += "panel cb { layout: column; padding: 8 12; gap: 8;\n";
    for (u32 pi = 0; pi < comp->prop_count; ++pi) {
      const scene::PropDesc &pd = comp->props[pi];
      scene::PropValue v;
      scene::GetProp(*world_, e, *comp, pd, &v);
      out += F("  panel r%u { layout: row; align: center; gap: 8;\n", pi);
      out += F("    text rl { class: row_label; text: \"%s\"; }\n",
               DisplayName(pd.name).c_str());
      int count =
          (v.type == scene::PropType::kVec3) ? 3
          : (v.type == scene::PropType::kVec4 || v.type == scene::PropType::kQuat)
              ? 3
          : (v.type == scene::PropType::kVec2) ? 2
                                              : 1;
      if (v.type == scene::PropType::kString) {
        const base::String escaped_value = EscapeUguiString(v.s);
        out += F("    panel fv { class: field; text vv { class: field_val; "
                 "text: \"%s\"; } }\n",
                 escaped_value.c_str());
      } else if (v.type == scene::PropType::kBool) {
        out += F("    button field_%u_%u_0 { width: 18; height: 18; "
                 "corner-radius: 4; "
                 "background: %s; cursor: pointer; }\n",
                 comp->id, pi, v.b ? "#3a5bbf" : "#33363c");
      } else if (v.type == scene::PropType::kU64 ||
                 v.type == scene::PropType::kAssetId ||
                 v.type == scene::PropType::kEntity) {
        base::String txt;
        if (v.type == scene::PropType::kEntity)
          txt = F("entity %u", v.e.index);
        else
          txt = F("%016llx", (unsigned long long)v.u);
        out += F("    panel fv { class: field; text vv { class: field_val; "
                 "color:#8fa9d6; text: \"%s\"; } }\n",
                 txt.c_str());
      } else {
        f32 disp[4] = {v.f[0], v.f[1], v.f[2], v.f[3]};
        if (v.type == scene::PropType::kQuat)
          QuatToEuler(Quat{v.f[0], v.f[1], v.f[2], v.f[3]}, disp);
        out += "    panel fields { layout: row; gap: 5; flex-grow: 1;\n";
        for (int a = 0; a < count; ++a) {
          out += F("      panel field_%u_%u_%d { class: field; cursor: move; "
                   "text fk { font-size: 11; font-weight: bold; color: %s; "
                   "text: \"%s\"; } "
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
  out += "  button insp_addcomp { text: \"+ Add Component\"; layout: row; "
         "justify: center; align:center; height: 30; "
         "background: #2c2f34; corner-radius: 6; border-color:#3a3d42; "
         "border-width:1; color:#a7aab0; font-size:12; cursor: pointer; :hover "
         "{ background:#3a3d44; } }\n";
  if (add_menu_open_) {
    out += "  panel addmenu { layout: column; margin: 6 0 0 0; background: "
           "#191a1d; corner-radius: 6; border-color:#33363c; border-width:1; "
           "padding: 4;\n";
    for (const scene::ComponentDesc *comp : scene::AllComponents()) {
      if (world_->HasRaw(e, comp->id))
        continue;
      if (base::String(comp->name) == "Guid")
        continue;
      if (base::String(comp->name) == "Name")
        continue; // ECS base::String unsafe; edited via side table
      out += F("    button insp_add_%u { text: \"%s\"; font-size: 12; "
               "color:#c9ccd2; padding: 7 10; corner-radius:4; cursor: "
               "pointer; :hover { background:#2c2f34; } }\n",
               comp->id, comp->name);
    }
    out += "  }\n";
  }
  out += "}\n";
  return out;
}

base::String Editor::BuildModeToolbar() {
  auto mode_button = [&](const char *id, const char *label, bool active,
                         int width) {
    return F("button %s { class: %s; width: %d; text: \"%s\"; }\n", id,
             active ? "tool_btn_on" : "tool_btn", width, label);
  };
  base::String out;
  out += mode_button("mode_select", "Select",
                     editor_mode_ == EditorMode::kSelect, 56);
  out += mode_button("mode_terrain", "Terrain",
                     editor_mode_ == EditorMode::kTerrain, 64);
  if (placement_.armed)
    out += mode_button("mode_place", "Place",
                       editor_mode_ == EditorMode::kPlace, 54);
  out += "panel mode_sep { width: 1; height: 22; background: #34373d; margin: "
         "0 5; }\n";
  out += mode_button("tool_translate", "Move",
                     editor_mode_ == EditorMode::kSelect &&
                         gizmo_mode_ == GizmoMode::kTranslate,
                     44);
  out += mode_button("tool_rotate", "Rot",
                     editor_mode_ == EditorMode::kSelect &&
                         gizmo_mode_ == GizmoMode::kRotate,
                     34);
  out += mode_button("tool_scale", "Scl",
                     editor_mode_ == EditorMode::kSelect &&
                         gizmo_mode_ == GizmoMode::kScale,
                     34);
  if (!imported_models_.empty()) {
    out += "panel walk_sep { width: 1; height: 22; background: #34373d; "
           "margin: 0 5; }\n";
    out += mode_button("walk_auto", "Walk Auto",
                       walk_preview_mode_ == WalkPreviewMode::kAuto, 72);
    out += mode_button("walk_hip_sway", "Hip Sway",
                       walk_preview_mode_ == WalkPreviewMode::kHipSway, 68);
    out += mode_button("walk_march", "March",
                       walk_preview_mode_ == WalkPreviewMode::kMarch, 54);
  }
  return out;
}

base::String Editor::BuildInspectorTabs() {
  if (editor_mode_ == EditorMode::kTerrain) {
    return "text terrain_tab { text: \"TERRAIN WORKSPACE\"; class: "
           "header_label; color: #d8e2ca; "
           "padding: 0 16; }\n";
  }
  if (editor_mode_ == EditorMode::kPlace) {
    return "text place_tab { text: \"PLACEMENT BRUSH\"; class: header_label; "
           "color: #e7cf9d; "
           "padding: 0 16; }\n";
  }
  base::String out;
  out += F("button tab_inspector { text: \"INSPECTOR\"; class: header_label; "
           "padding: 0 16; "
           "height: 32; layout: row; align: center; cursor: pointer; color: "
           "%s; %s }\n",
           material_tab_ ? "#8a8d93" : "#e6e8ec",
           material_tab_ ? ":hover { color: #c9ccd2; }"
                         : "border-color: #5a8dee; border-width: 0 0 2 0;");
  out += F("button tab_material { text: \"MATERIAL\"; class: header_label; "
           "padding: 0 16; "
           "height: 32; layout: row; align: center; cursor: pointer; color: "
           "%s; %s }\n",
           material_tab_ ? "#e6e8ec" : "#8a8d93",
           material_tab_ ? "border-color: #5a8dee; border-width: 0 0 2 0;"
                         : ":hover { color: #c9ccd2; }");
  return out;
}

base::String Editor::BuildTerrainInspector() {
  const char *tool_names[] = {"Raise", "Lower", "Smooth", "Flatten", "Paint"};
  const terrain::TerrainBrushMode tool_modes[] = {
      terrain::TerrainBrushMode::kRaise, terrain::TerrainBrushMode::kLower,
      terrain::TerrainBrushMode::kSmooth, terrain::TerrainBrushMode::kFlatten,
      terrain::TerrainBrushMode::kPaintLayer};
  const char *tool_ids[] = {"terrain_raise", "terrain_lower", "terrain_smooth",
                            "terrain_flatten", "terrain_paint"};
  base::String out;
  out += "panel terrain_asset { layout: column; padding: 12; gap: 6; "
         "background: #1c211d; "
         "border-color: #344238; border-width: 0 0 1 0;\n";
  out += "  panel ta_head { layout: row; align: center; gap: 8; panel dot { "
         "width: 9; height: 9; "
         "corner-radius: 5; background: #7fa35d; } text title { text: \"ACTIVE "
         "TERRAIN\"; "
         "font-size: 11; font-weight: bold; letter-spacing: 1; color: #cbd8c0; "
         "} }\n";
  const base::String escaped_path = EscapeUguiString(terrain_path_);
  out += F("  text ta_path { text: \"%s\"; font-size: 11; color: #8e9b8a; }\n",
           escaped_path.c_str());
  out += F("  text ta_state { text: \"%s\"; font-size: 11; color: %s; }\n",
           terrain_dirty_ ? "Unsaved terrain edits" : "Saved terrain asset",
           terrain_dirty_ ? "#e0a05a" : "#72bc83");
  out += "}\n";
  out += "panel terrain_tools { layout: column; padding: 12; gap: 8; "
         "text tt { class: header_label; text: \"SCULPT + PAINT\"; }\n"
         "  panel tool_row { layout: row; gap: 5; flex-wrap: wrap;\n";
  for (int i = 0; i < 5; ++i) {
    const bool active = terrain_brush_mode_ == tool_modes[i];
    out += F("    button %s { text: \"%s\"; font-size: 11; color: %s; "
             "background: %s; "
             "border-color: %s; border-width: 1; corner-radius: 6; padding: 7 "
             "9; cursor: pointer; "
             ":hover { background: #3a3d44; } }\n",
             tool_ids[i], tool_names[i], active ? "#ffffff" : "#b7bac0",
             active ? "#3d5f49" : "#292c30", active ? "#78a86c" : "#3a3d42");
  }
  out += "  }\n}\n";

  auto control = [&](const char *label, const char *id, f32 value) {
    return F("  panel %s_row { layout: row; align: center; gap: 7; text l { "
             "class: row_label; "
             "text: \"%s\"; } button terrain_%s_minus { text: \"-\"; class: "
             "tool_btn; width: 27; "
             "height: 27; } panel v { class: field; justify: center; text n { "
             "text: \"%.2f\"; "
             "font-size: 12; color: #d8dbdf; } } button terrain_%s_plus { "
             "text: \"+\"; "
             "class: tool_btn; width: 27; height: 27; } }\n",
             id, label, id, value, id);
  };
  out += "panel terrain_brush { layout: column; padding: 10 12; gap: 7; "
         "background: #202226; border-color: #101113; border-width: 1 0;\n";
  out += control("Radius", "radius", terrain_brush_radius_);
  out += control("Strength", "strength", terrain_brush_strength_);
  out += control("Falloff", "falloff", terrain_brush_falloff_);
  out += "}\n";

  out += "panel palette { layout: column; padding: 12; gap: 8; text ph { "
         "class: header_label; "
         "text: \"LAYER PALETTE\"; }\n";
  for (u32 i = 0; i < terrain_.desc().layers.size(); ++i) {
    const terrain::TerrainLayer &layer = terrain_.desc().layers[i];
    const auto &c = layer.debug_rgba;
    const base::String escaped_name = EscapeUguiString(layer.name);
    out += F("  button terrain_layer_%u { layout: row; align: center; gap: 10; "
             "height: 34; "
             "padding: 0 8; background: %s; corner-radius: 6; border-color: "
             "%s; border-width: 1; "
             "cursor: pointer; :hover { background: #2e3235; } panel sw { "
             "width: 20; height: 20; "
             "corner-radius: 5; background: #%02x%02x%02x; border-color: "
             "#ffffff33; border-width: 1; } "
             "text nm { text: \"%s\"; font-size: 12; color: %s; } }\n",
             i, i == terrain_brush_layer_ ? "#2c3830" : "#24262a",
             i == terrain_brush_layer_ ? "#78a86c" : "#34373d", c[0], c[1],
             c[2], escaped_name.c_str(),
             i == terrain_brush_layer_ ? "#eef4ea" : "#b7bac0");
  }
  out += "}\n";
  out += "panel terrain_hint { layout: column; gap: 5; margin: 2 12 12 12; "
         "padding: 10; "
         "background: #191d1a; corner-radius: 7; border-color: #334037; "
         "border-width: 1; "
         "text h1 { text: \"LMB drag paints spaced live dabs\"; font-size: 11; "
         "color: #aeb9aa; } "
         "text h2 { text: \"Shift inverts Raise / Lower\"; font-size: 11; "
         "color: #778373; } "
         "text h3 { text: \"RMB fly camera  |  Esc Select\"; font-size: 11; "
         "color: #778373; } }\n";
  return out;
}

base::String Editor::BuildPlacementInspector() {
  base::String out;
  const base::String escaped_name = EscapeUguiString(placement_.name);
  out += "panel place_asset { layout: column; padding: 14; gap: 7; background: "
         "#211f19; "
         "border-color: #4a402d; border-width: 0 0 1 0;\n";
  out += "  text pa_k { class: header_label; text: \"ARMED ASSET\"; color: "
         "#c9ad76; }\n";
  out += F("  text pa_n { text: \"%s\"; font-size: 16; font-weight: bold; "
           "color: #f0e6d1; }\n",
           escaped_name.c_str());
  out += "  text pa_s { text: \"Persistent surface placement\"; font-size: 11; "
         "color: #9b907c; }\n"
         "}\n";
  out += "panel place_settings { layout: column; padding: 12; gap: 10; text "
         "ps_h { class: header_label; "
         "text: \"BRUSH SETTINGS\"; } panel spacing { layout: row; align: "
         "center; gap: 8; "
         "text l { class: row_label; text: \"Spacing\"; } button "
         "place_spacing_minus { text: \"-\"; "
         "class: tool_btn; width: 28; height: 28; } panel v { class: field; "
         "justify: center; "
         "text n { text: \"";
  out += F("%.2f", placement_.spacing);
  out += " m\"; font-size: 12; color: #e7cf9d; } } button place_spacing_plus { "
         "text: \"+\"; "
         "class: tool_btn; width: 28; height: 28; } } }\n";
  out += "panel place_hint { layout: column; gap: 6; margin: 0 12; padding: "
         "11; background: #1d1b17; "
         "corner-radius: 7; border-color: #453d2d; border-width: 1; "
         "text h1 { text: \"LMB drag places one undo-grouped stroke\"; "
         "font-size: 11; color: #b9ad96; } "
         "text h2 { text: \"Copies snap to the terrain surface\"; font-size: "
         "11; color: #867c69; } "
         "text h3 { text: \"RMB fly  |  Esc or Select cancels\"; font-size: "
         "11; color: #867c69; } }\n";
  return out;
}

base::String Editor::BuildContent() {
  base::String out;
  size_t idx = 0;
  for (const AssetEntry &a : assets_list_) {
    if (!content_filter_.empty() &&
        a.name.find(content_filter_) == base::String::npos) {
      ++idx;
      continue;
    }
    out += F(
        "panel card_%zu { layout: column; align: center; gap: 6; width: 96;\n",
        idx);
    out += F("  button asset_%zu { width: 96; height: 80; corner-radius: 8; "
             "layout: row; justify: center; align: center; "
             "background: #191a1d; border-color: %s; border-width: 1; cursor: "
             "pointer; :hover { background: #22242a; }\n"
             "    text bt { text: \"%s\"; font-size: 11; font-weight: bold; "
             "color: %s; } }\n",
             idx, KindColor(a.kind).c_str(), a.kind.c_str(),
             KindColor(a.kind).c_str());
    const base::String escaped_name = EscapeUguiString(a.name);
    out += F("  text cn { text: \"%s\"; font-size: 11; color: #b7bac0; }\n",
             escaped_name.c_str());
    out += "}\n";
    ++idx;
  }
  if (out.empty())
    out = "text ce { text: \"(no assets)\"; font-size: 12; color:#5a5d63; "
          "padding: 8; }\n";
  return out;
}

base::String Editor::BuildDialog() {
  if (!dialog_open_)
    return "";
  base::String out = "panel dialog_scrim { position: absolute; left: 0; top: 0; "
                    "width: 100vw; height: 100vh; "
                    "layout: column; justify: center; align: center; "
                    "background: #000000aa;\n";
  out += "  panel dialog { layout: column; width: 460; background: #26282c; "
         "corner-radius: 10; "
         "border-color:#3a3d42; border-width:1; padding: 0;\n";
  out += "    panel dh { class: panel_header; text dt { class: header_label; "
         "text: \"OPEN SCENE\"; } }\n";
  out += "    panel dl { layout: column; padding: 10; gap: 3; max-height: 360; "
         "overflow: scroll;\n";
  if (dialog_files_.empty())
    out += "      text de { text: \"No .rxscene files found\"; font-size: 12; "
           "color:#6a6d73; padding: 8; }\n";
  for (size_t i = 0; i < dialog_files_.size(); ++i) {
    const base::String escaped_path = EscapeUguiString(dialog_files_[i]);
    out += F("      button dlg_%zu { text: \"%s\"; font-size: 13; "
             "color:#c9ccd2; padding: 8 10; corner-radius: 5; "
             "cursor: pointer; :hover { background:#2f4368; color:#fff; } }\n",
             i, escaped_path.c_str());
  }
  out += "    }\n";
  out += "    panel df { layout: row; justify: end; padding: 10; gap: 8; "
         "border-color:#101113; border-width:1 0 0 0;\n"
         "      button dlg_cancel { text: \"Cancel\"; font-size: 12; "
         "color:#c9ccd2; padding: 8 16; background:#2c2f34; corner-radius:6; "
         "cursor: pointer; :hover { background:#3a3d44; } } }\n";
  out += "  }\n}\n";
  return out;
}

void Editor::UiHotReloadCheck(f32 dt) {
  reload_timer_ += dt;
  if (reload_timer_ < 0.3f)
    return;
  reload_timer_ = 0;
  base::String path =
      (ui_dir_.empty() ? base::String("apps/editor/ui") : ui_dir_) +
      "/editor.ugui";
  const base::Optional<i64> t = fs::LastWriteTime(path);
  if (!t)
    return;
  int64_t m = *t;
  if (m != ui_mtime_ && ui_mtime_ != 0)
    MarkDirty();
}

// Event routing
namespace {
// Climb up to `max` ancestors, returning the first non-empty widget name.
base::String NamedAncestor(ugui::UIContext &ui, ugui::wid w, int max = 5) {
  for (int i = 0; i < max && w.valid(); ++i) {
    if (ugui::WidgetNode *n = ui.world().Get<ugui::WidgetNode>(w)) {
      if (!n->name.empty())
        return base::String(n->name.c_str());
    }
    ugui::Hierarchy *h = ui.world().Get<ugui::Hierarchy>(w);
    w = h ? h->parent : ugui::wid{};
  }
  return "";
}
} // namespace

void Editor::OnUiClick(ugui::wid w, ugui::MouseButton btn) {
  base::String name = NamedAncestor(ui_, w);
  if (name.empty())
    return;
  RouteClick(name, btn);
}

bool Editor::RouteClick(const base::String &name, ugui::MouseButton) {
  auto starts = [&](const char *p) { return name.rfind(p, 0) == 0; };
  auto tail_u = [&](const char *p) -> unsigned {
    return (unsigned)::strtoul(name.c_str() + ::strlen(p), nullptr, 10);
  };

  if (name == "menu_new") {
    NewScene();
    return true;
  }
  if (name == "menu_open") {
    OpenFileDialog();
    return true;
  }
  if (name == "menu_save") {
    if (scene_path_ == "untitled.rxscene")
      OpenFileDialog();
    else
      DoSave(scene_path_);
    return true;
  }
  if (name == "menu_bakeworld") {
    DoBakeWorld();
    return true;
  }
  if (name == "menu_saveas") {
    DoSave("scene_saved.rxscene");
    return true;
  }
  if (name == "btn_undo") {
    PerformUndo();
    return true;
  }
  if (name == "btn_redo") {
    PerformRedo();
    return true;
  }
  if (name == "btn_play") {
    playing_ = true;
    return true;
  }
  if (name == "btn_stop") {
    playing_ = false;
    return true;
  }
  if (name == "walk_auto" || name == "walk_hip_sway" ||
      name == "walk_march") {
    walk_preview_mode_ = name == "walk_auto"
                             ? WalkPreviewMode::kAuto
                         : name == "walk_hip_sway"
                             ? WalkPreviewMode::kHipSway
                             : WalkPreviewMode::kMarch;
    status_message_ = name == "walk_auto"
                          ? "Walk preview cycles Hip Sway and March"
                      : name == "walk_hip_sway"
                          ? "Walk preview: Hip Sway"
                          : "Walk preview: March";
    MarkDirty();
    return true;
  }
  if (name == "mode_select") {
    SetEditorMode(EditorMode::kSelect);
    return true;
  }
  if (name == "mode_terrain") {
    SetEditorMode(EditorMode::kTerrain);
    return true;
  }
  if (name == "mode_place" && placement_.armed) {
    SetEditorMode(EditorMode::kPlace);
    return true;
  }
  if (name == "tool_translate") {
    SetEditorMode(EditorMode::kSelect);
    gizmo_mode_ = GizmoMode::kTranslate;
    MarkDirty();
    return true;
  }
  if (name == "tool_rotate") {
    SetEditorMode(EditorMode::kSelect);
    gizmo_mode_ = GizmoMode::kRotate;
    MarkDirty();
    return true;
  }
  if (name == "tool_scale") {
    SetEditorMode(EditorMode::kSelect);
    gizmo_mode_ = GizmoMode::kScale;
    MarkDirty();
    return true;
  }
  if (name == "tab_inspector") {
    material_tab_ = false;
    MarkDirty();
    return true;
  }
  if (name == "tab_material") {
    material_tab_ = true;
    MarkDirty();
    return true;
  }
  if (name == "terrain_raise") {
    terrain_brush_mode_ = terrain::TerrainBrushMode::kRaise;
    MarkDirty();
    return true;
  }
  if (name == "terrain_lower") {
    terrain_brush_mode_ = terrain::TerrainBrushMode::kLower;
    MarkDirty();
    return true;
  }
  if (name == "terrain_smooth") {
    terrain_brush_mode_ = terrain::TerrainBrushMode::kSmooth;
    MarkDirty();
    return true;
  }
  if (name == "terrain_flatten") {
    terrain_brush_mode_ = terrain::TerrainBrushMode::kFlatten;
    MarkDirty();
    return true;
  }
  if (name == "terrain_paint") {
    terrain_brush_mode_ = terrain::TerrainBrushMode::kPaintLayer;
    MarkDirty();
    return true;
  }
  if (name == "terrain_radius_minus") {
    terrain_brush_radius_ = rx::Max(0.25f, terrain_brush_radius_ - 0.25f);
    MarkDirty();
    return true;
  }
  if (name == "terrain_radius_plus") {
    terrain_brush_radius_ = rx::Min(24.0f, terrain_brush_radius_ + 0.25f);
    MarkDirty();
    return true;
  }
  if (name == "terrain_strength_minus") {
    terrain_brush_strength_ = rx::Max(0.01f, terrain_brush_strength_ - 0.05f);
    MarkDirty();
    return true;
  }
  if (name == "terrain_strength_plus") {
    terrain_brush_strength_ = rx::Min(2.0f, terrain_brush_strength_ + 0.05f);
    MarkDirty();
    return true;
  }
  if (name == "terrain_falloff_minus") {
    terrain_brush_falloff_ = rx::Max(0.0f, terrain_brush_falloff_ - 0.25f);
    MarkDirty();
    return true;
  }
  if (name == "terrain_falloff_plus") {
    terrain_brush_falloff_ = rx::Min(8.0f, terrain_brush_falloff_ + 0.25f);
    MarkDirty();
    return true;
  }
  if (starts("terrain_layer_")) {
    const u32 last_layer =
        terrain_.desc().layers.empty()
            ? 0
            : static_cast<u32>(terrain_.desc().layers.size() - 1);
    terrain_brush_layer_ = rx::Min<u32>(tail_u("terrain_layer_"), last_layer);
    terrain_brush_mode_ = terrain::TerrainBrushMode::kPaintLayer;
    MarkDirty();
    return true;
  }
  if (name == "place_spacing_minus") {
    placement_.spacing = rx::Max(0.25f, placement_.spacing - 0.25f);
    MarkDirty();
    return true;
  }
  if (name == "place_spacing_plus") {
    placement_.spacing = rx::Min(20.0f, placement_.spacing + 0.25f);
    MarkDirty();
    return true;
  }

  if (name == "hier_add") {
    ecs::Entity out;
    const scene::ComponentDesc *xf = scene::FindComponentByName("Transform");
    const scene::ComponentDesc *nm = scene::FindComponentByName("Name");
    base::Vector<base::Pair<
        const scene::ComponentDesc *,
        base::Vector<base::Pair<const scene::PropDesc *, scene::PropValue>>>>
        initial;
    initial.push_back({xf, {}});
    initial.push_back(
        {nm, {{&nm->props[0], scene::PropValue::String("Empty")}}});
    undo_.Push(*world_, edit::MakeCreateEntity(base::move(initial), &out));
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
    if (::sscanf(name.c_str() + 5, "%u_%u", &index, &gen) == 2) {
      ecs::Entity e{index, gen};
      if (world_->IsAlive(e)) {
        selection_.Set(e);
        MarkDirty();
      }
    }
    return true;
  }
  if (starts("insp_addcomp")) {
    add_menu_open_ = !add_menu_open_;
    MarkDirty();
    return true;
  }
  if (starts("insp_add_")) {
    ecs::ComponentId id = tail_u("insp_add_");
    if (const scene::ComponentDesc *c = scene::FindComponent(id))
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
    if (const scene::ComponentDesc *c = scene::FindComponent(id))
      if (ecs::Entity e = selection_.primary()) {
        undo_.Push(*world_, edit::MakeRemoveComponent(*world_, e, *c));
        doc_dirty_ = true;
        MarkDirty();
      }
    return true;
  }
  // toggle bool field (field_<compid>_<prop>_0 on a bool)
  if (starts("field_") && name.find("_tint_") == base::String::npos) {
    unsigned cid = 0, pi = 0, ax = 0;
    if (::sscanf(name.c_str() + 6, "%u_%u_%u", &cid, &pi, &ax) == 3) {
      const scene::ComponentDesc *c = scene::FindComponent(cid);
      ecs::Entity e = selection_.primary();
      if (c && e && pi < c->prop_count &&
          c->props[pi].type == scene::PropType::kBool) {
        scene::PropValue v;
        scene::GetProp(*world_, e, *c, c->props[pi], &v);
        undo_.Push(*world_, edit::MakeSetProp(*world_, e, *c, c->props[pi],
                                              scene::PropValue::Bool(!v.b)));
        doc_dirty_ = true;
        MarkDirty();
      }
    }
    return true;
  }
  if (starts("asset_")) {
    unsigned i = tail_u("asset_");
    if (i < assets_list_.size()) {
      const AssetEntry &a = assets_list_[i];
      if (a.kind == "scene") {
        DoLoad(a.path);
      } else if (a.kind == "model") {
        OpenDocument(a.path);
      } else if (a.kind == "terrain") {
        LoadTerrainAsset(a.path);
      } else if (a.kind == "mesh") {
        ArmPlacement(a);
      }
    }
    return true;
  }
  if (starts("dlg_cancel")) {
    dialog_open_ = false;
    MarkDirty();
    return true;
  }
  if (starts("dlg_")) {
    unsigned i = tail_u("dlg_");
    if (i < dialog_files_.size()) {
      OpenDocument(dialog_files_[i]);
      dialog_open_ = false;
    }
    return true;
  }
  return false;
}

void Editor::OnUiTextSubmit(const base::String &widget,
                            const base::String &value) {
  if (widget == "insp_name") {
    if (ecs::Entity e = selection_.primary()) {
      SetName(e, value); // side table (ECS base::String is unsafe today)
      doc_dirty_ = true;
      MarkDirty();
    }
  }
}

// OnBuildView: gather draws + drive the UI production
void Editor::OnBuildView(f32 dt, render::FrameView &view) {
  view.camera.eye = camera_.position();
  view.camera.target = camera_.target();
  view.frame_delta_seconds = dt > 0 ? dt : 1.0f / 60.0f;
  UpdateImportedModels(view.frame_delta_seconds);

  // Manual gather so we can apply parent transforms, per-entity/selection tint
  // and the pick ids the GPU pick pass writes.
  ecs::Entity primary = selection_.primary();
  pick_map_.clear();
  u32 next_pick_id = 1; // 0 = background/unpickable
  world_->Each<scene::Transform, scene::Renderable>(
      [&](ecs::Entity e, scene::Transform &, scene::Renderable &r) {
        if (world_->Has<scene::Hidden>(e))
          return;
        const bool terrain_visual = IsTerrainVisual(e);
        render::DrawItem d;
        d.mesh = r.mesh.hash;
        d.transform = scene::WorldMatrix(*world_, e);
        d.prev_transform = d.transform;
        uint32_t tint = terrain_visual
                            ? 0u
                            : (tints_.contains(e.index) ? tints_[e.index] : 0u);
        if (!terrain_visual && e == primary)
          tint = 0xffa64d; // selection highlight
        d.tint = tint;
        const base::Pair<u32, u32> *imported =
            imported_entities_.find(ImportedEntityKey(e));
        if (imported) {
          const auto [model_index, instance_index] = *imported;
          if (model_index < imported_models_.size() &&
              instance_index < imported_models_[model_index].instances.size()) {
            ImportedModel &model = imported_models_[model_index];
            ImportedInstance &instance = model.instances[instance_index];
            const MeshRecord *record = FindMesh(instance.mesh);
            if (record && instance.skin >= 0 &&
                instance.skin < static_cast<i32>(model.skins.size())) {
              ImportedSkin &skin = model.skins[instance.skin];
              anim::BuildSkinPalette(skin.model_matrices, skin.binding,
                                     instance.remap, &instance.palette);
              d.skin_offset = static_cast<i32>(view.bone_matrices.size());
              for (const Mat4 &matrix : instance.palette)
                view.bone_matrices.push_back(matrix);

              instance.morph_weights.resize(record->mesh.morph_targets.size());
              base::Fill(instance.morph_weights.begin(),
                         instance.morph_weights.end(), 0.0f);
              anim::ApplyBodyMorphWeights(record->mesh, skin.morphs,
                                          &instance.morph_weights);
              d.morph_offset = static_cast<i32>(view.morph_weights.size());
              d.morph_count = anim::AppendActiveMorphWeights(
                  instance.morph_weights, &view.morph_weights);
              if (d.morph_count == 0)
                d.morph_offset = -1;
            }
          }
        }
        if (!terrain_visual) {
          d.pick_id = next_pick_id;
          pick_map_[next_pick_id] = e;
          ++next_pick_id;
        }
        view.draws.push_back(d);
      });

  // 1m ground grid through the depth-tested debug-line pass.
  if (grid_lines_.empty()) {
    const int n = 10;
    for (int i = -n; i <= n; ++i) {
      u32 c = i == 0 ? 0x5a5d63ff : 0x33363caa;
      grid_lines_.push_back(
          {{(f32)i, 0.011f, (f32)-n}, {(f32)i, 0.011f, (f32)n}, c});
      grid_lines_.push_back(
          {{(f32)-n, 0.011f, (f32)i}, {(f32)n, 0.011f, (f32)i}, c});
    }
  }
  view.debug_lines = grid_lines_;

  // Translate-gizmo axis lines, drawn on top of the scene.
  gizmo_lines_.clear();
  if (editor_mode_ == EditorMode::kSelect && primary &&
      world_->IsAlive(primary) && !IsTerrainVisual(primary) &&
      gizmo_mode_ == GizmoMode::kTranslate) {
    scene::Transform wt = scene::WorldTransform(*world_, primary);
    Vec3 o{wt.position[0], wt.position[1], wt.position[2]};
    f32 len = rx::Max(0.5f, Length(o - camera_.position()) * 0.18f);
    gizmo_lines_.push_back({o, {o.x + len, o.y, o.z}, 0xe0655fff}); // X red
    gizmo_lines_.push_back({o, {o.x, o.y + len, o.z}, 0x7fb96aff}); // Y green
    gizmo_lines_.push_back({o, {o.x, o.y, o.z + len}, 0x5a8deeff}); // Z blue
  }
  AppendInteractionPreview(&gizmo_lines_);
  view.debug_lines_overlay = gizmo_lines_;

  if (!ui_ready_)
    return;
  if (ui_dirty_)
    UiRebuild();
  UiPerFrameText();
  UpdateGizmoWidgets();

  draw_data_ = &ui_.RenderDrawData();
  view.needs_blur = false;
  for (u32 i = 0; i < draw_data_->command_count; ++i)
    if (draw_data_->commands[i].blur > 0) {
      view.needs_blur = true;
      break;
    }

  if (ui_.text_engine().atlas_revision() != font_revision_) {
    ugui::Vec2 as = ui_.text_engine().atlas_size();
    backend_.UpdateFontAtlas(ui_.text_engine().atlas_pixels(), (u32)as.x,
                             (u32)as.y);
    font_revision_ = ui_.text_engine().atlas_revision();
  }
  backend_.NewFrame();
  view.hud_draw = [this, &view](gpu::CommandList &cmd) {
    backend_.SetBackdrop(gpu::GetVkImageView(view.blur_source),
                         gpu::GetVkSampler(view.blur_sampler));
    if (draw_data_)
      backend_.Render(*draw_data_, gpu::GetVkCommandBuffer(cmd));
  };
}

// scrub helpers implemented here (need ugui hovered name + reflect)
bool Editor::TryStartScrub(f32 mx) {
  base::String name = NamedAncestor(ui_, ui_.input().hovered_widget());
  if (name.rfind("field_", 0) != 0)
    return false;
  unsigned cid = 0, pi = 0, ax = 0;
  if (name.find("_tint_") != base::String::npos) {
    if (::sscanf(name.c_str() + ::strlen("field_tint_"), "%u_%u", &pi,
                    &ax) == 2) {
      // tint scrub: live edit of tints_ (not undo-tracked)
      scrub_ = {};
      scrub_.active = true;
      scrub_.entity = selection_.primary();
      scrub_.comp = nullptr; // signals tint
      scrub_.axis = (int)ax;
      scrub_.start_mouse = mx;
      uint32_t t = tints_.contains(scrub_.entity.index)
                       ? tints_[scrub_.entity.index]
                       : 0xffffff;
      scrub_.base_value = (f32)((t >> (16 - ax * 8)) & 0xFF);
      scrub_.step = 1.0f;
      return true;
    }
    return false;
  }
  if (::sscanf(name.c_str() + 6, "%u_%u_%u", &cid, &pi, &ax) != 3)
    return false;
  const scene::ComponentDesc *c = scene::FindComponent(cid);
  ecs::Entity e = selection_.primary();
  if (!c || !e || pi >= c->prop_count)
    return false;
  const scene::PropDesc &pd = c->props[pi];
  if (pd.type == scene::PropType::kBool || pd.type == scene::PropType::kString)
    return false;
  scene::PropValue v;
  scene::GetProp(*world_, e, *c, pd, &v);
  scrub_ = {};
  scrub_.active = true;
  scrub_.entity = e;
  scrub_.comp = c;
  scrub_.prop = &pd;
  scrub_.axis = (int)ax;
  scrub_.start_mouse = mx;
  scrub_.step = (pd.type == scene::PropType::kQuat) ? 0.5f : 0.02f;
  if (pd.type == scene::PropType::kQuat) {
    f32 euler[3];
    QuatToEuler(Quat{v.f[0], v.f[1], v.f[2], v.f[3]}, euler);
    scrub_.base_value = euler[ax];
    scrub_.base_euler[0] = euler[0];
    scrub_.base_euler[1] = euler[1];
    scrub_.base_euler[2] = euler[2];
  } else {
    scrub_.base_value = v.f[ax];
  }
  undo_.BeginGroup("Edit");
  return true;
}

void Editor::UpdateScrub() {
  if (!scrub_.active)
    return;
  const InputState &in = window_->input();
  f32 dx = in.mouse_x - scrub_.start_mouse;
  f32 nv = scrub_.base_value + dx * scrub_.step;

  if (!scrub_.comp) { // tint live edit
    ecs::Entity e = scrub_.entity;
    int iv = (int)rx::Clamp(nv, 0.0f, 255.0f);
    uint32_t t = tints_.contains(e.index) ? tints_[e.index] : 0xffffff;
    int shift = 16 - scrub_.axis * 8;
    t = (t & ~(0xFFu << shift)) | ((uint32_t)iv << shift);
    tints_[e.index] = t;
    MarkDirty();
    return;
  }

  const scene::PropDesc &pd = *scrub_.prop;
  scene::PropValue v;
  scene::GetProp(*world_, scrub_.entity, *scrub_.comp, pd, &v);
  if (pd.type == scene::PropType::kQuat) {
    f32 euler[3] = {scrub_.base_euler[0], scrub_.base_euler[1],
                    scrub_.base_euler[2]};
    euler[scrub_.axis] = scrub_.base_value + dx * scrub_.step;
    Quat q = EulerToQuat(euler);
    undo_.Push(*world_,
               edit::MakeSetProp(*world_, scrub_.entity, *scrub_.comp, pd,
                                 scene::PropValue::Quat(q.x, q.y, q.z, q.w)));
  } else {
    if (pd.min != pd.max)
      nv = rx::Clamp(nv, pd.min, pd.max);
    scene::PropValue out = v;
    out.f[scrub_.axis] = nv;
    undo_.Push(*world_, edit::MakeSetProp(*world_, scrub_.entity, *scrub_.comp,
                                          pd, out));
  }
  doc_dirty_ = true;
  MarkDirty();
}

} // namespace rx::editor
