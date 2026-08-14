#ifndef RX_RENDER_UTIL_IMGUI_THEME_H_
#define RX_RENDER_UTIL_IMGUI_THEME_H_

// rx's Dear ImGui look: a near-black violet base with a purple accent, sized
// and rounded for the debug overlays, and translucent throughout so the
// renderer's frosted backdrop (ImGuiRenderer::SetBackdrop) reads as glass
// rather than as a grey wash. Pair it with LoadRxImGuiFont for Roboto.
//
// Header-only on purpose. Under RX_SHARED the render module is its own DSO with
// its own copy of imgui core, while the app keeps the live ImGui context; a
// function compiled into rx_render would set the style on the wrong one. Inline
// here, it compiles into the caller and touches the caller's context - the same
// reason ImGuiRenderer itself calls no global ImGui:: function.

#include <cstddef>
#include <cstring>

#include <imgui.h>

#include "core/types.h"

namespace rx::render {

// The engine's default UI font, in the archive the host mounts at fonts://
// (asset::MountEngineArchives).
constexpr const char* kRxDefaultFontPath = "fonts://roboto/Roboto-Regular.ttf";
constexpr f32 kRxDefaultFontSize = 16.0f;

// Applies the rx palette and metrics to `style`.
inline void ApplyRxImGuiStyle(ImGuiStyle& style) {
  ImGui::StyleColorsDark(&style);  // base, so a colour added by a future imgui still lands sane

  // Palette. Backgrounds keep a violet cast rather than going neutral grey, so
  // the accent reads as the same family rather than as a sticker on top.
  const ImVec4 accent{0.647f, 0.353f, 0.980f, 1.0f};  // #a55afa
  const ImVec4 accent_bright{0.780f, 0.565f, 1.000f, 1.0f};
  const ImVec4 accent_dim{0.400f, 0.216f, 0.647f, 1.0f};
  const ImVec4 text{0.902f, 0.886f, 0.949f, 1.0f};
  const ImVec4 text_dim{0.478f, 0.451f, 0.565f, 1.0f};
  const ImVec4 base{0.047f, 0.039f, 0.078f, 1.0f};   // window body
  const ImVec4 raised{0.098f, 0.082f, 0.145f, 1.0f}; // frames, scrollbars
  const ImVec4 sunken{0.031f, 0.024f, 0.055f, 1.0f}; // title bars, tracks

  // Alpha is what the frosted backdrop shows through; opaque entries here are
  // the ones that must stay legible over any scene (text, marks, borders).
  auto with_alpha = [](ImVec4 c, f32 a) { return ImVec4{c.x, c.y, c.z, a}; };
  ImVec4* c = style.Colors;
  c[ImGuiCol_Text] = text;
  c[ImGuiCol_TextDisabled] = text_dim;
  c[ImGuiCol_WindowBg] = with_alpha(base, 0.82f);
  c[ImGuiCol_ChildBg] = with_alpha(raised, 0.30f);
  c[ImGuiCol_PopupBg] = with_alpha(base, 0.94f);
  c[ImGuiCol_Border] = with_alpha(accent, 0.28f);
  c[ImGuiCol_BorderShadow] = ImVec4{0, 0, 0, 0};
  c[ImGuiCol_FrameBg] = with_alpha(raised, 0.62f);
  c[ImGuiCol_FrameBgHovered] = with_alpha(accent, 0.28f);
  c[ImGuiCol_FrameBgActive] = with_alpha(accent, 0.42f);
  c[ImGuiCol_TitleBg] = with_alpha(sunken, 0.88f);
  c[ImGuiCol_TitleBgActive] = with_alpha(accent_dim, 0.75f);
  c[ImGuiCol_TitleBgCollapsed] = with_alpha(sunken, 0.70f);
  c[ImGuiCol_MenuBarBg] = with_alpha(sunken, 0.75f);
  c[ImGuiCol_ScrollbarBg] = with_alpha(sunken, 0.45f);
  c[ImGuiCol_ScrollbarGrab] = with_alpha(accent, 0.35f);
  c[ImGuiCol_ScrollbarGrabHovered] = with_alpha(accent, 0.55f);
  c[ImGuiCol_ScrollbarGrabActive] = with_alpha(accent, 0.80f);
  c[ImGuiCol_CheckMark] = accent_bright;
  c[ImGuiCol_CheckboxSelectedBg] = with_alpha(accent, 0.45f);
  c[ImGuiCol_SliderGrab] = with_alpha(accent, 0.90f);
  c[ImGuiCol_SliderGrabActive] = accent_bright;
  c[ImGuiCol_Button] = with_alpha(accent, 0.26f);
  c[ImGuiCol_ButtonHovered] = with_alpha(accent, 0.52f);
  c[ImGuiCol_ButtonActive] = with_alpha(accent, 0.76f);
  c[ImGuiCol_Header] = with_alpha(accent, 0.34f);
  c[ImGuiCol_HeaderHovered] = with_alpha(accent, 0.52f);
  c[ImGuiCol_HeaderActive] = with_alpha(accent, 0.72f);
  c[ImGuiCol_Separator] = with_alpha(accent, 0.30f);
  c[ImGuiCol_SeparatorHovered] = with_alpha(accent, 0.60f);
  c[ImGuiCol_SeparatorActive] = accent_bright;
  c[ImGuiCol_ResizeGrip] = with_alpha(accent, 0.22f);
  c[ImGuiCol_ResizeGripHovered] = with_alpha(accent, 0.50f);
  c[ImGuiCol_ResizeGripActive] = with_alpha(accent, 0.80f);
  c[ImGuiCol_InputTextCursor] = accent_bright;
  c[ImGuiCol_Tab] = with_alpha(raised, 0.55f);
  c[ImGuiCol_TabHovered] = with_alpha(accent, 0.50f);
  c[ImGuiCol_TabSelected] = with_alpha(accent_dim, 0.85f);
  c[ImGuiCol_TabSelectedOverline] = accent_bright;
  c[ImGuiCol_TabDimmed] = with_alpha(raised, 0.35f);
  c[ImGuiCol_TabDimmedSelected] = with_alpha(accent_dim, 0.50f);
  c[ImGuiCol_TabDimmedSelectedOverline] = with_alpha(accent, 0.40f);
  c[ImGuiCol_DockingPreview] = with_alpha(accent, 0.45f);
  c[ImGuiCol_DockingEmptyBg] = with_alpha(sunken, 0.85f);
  c[ImGuiCol_PlotLines] = accent_bright;
  c[ImGuiCol_PlotLinesHovered] = ImVec4{1.000f, 0.451f, 0.851f, 1.0f};
  c[ImGuiCol_PlotHistogram] = with_alpha(accent, 0.90f);
  c[ImGuiCol_PlotHistogramHovered] = accent_bright;
  c[ImGuiCol_TableHeaderBg] = with_alpha(accent_dim, 0.45f);
  c[ImGuiCol_TableBorderStrong] = with_alpha(accent, 0.35f);
  c[ImGuiCol_TableBorderLight] = with_alpha(accent, 0.16f);
  c[ImGuiCol_TableRowBg] = ImVec4{0, 0, 0, 0};
  c[ImGuiCol_TableRowBgAlt] = ImVec4{1.0f, 1.0f, 1.0f, 0.035f};
  c[ImGuiCol_TextLink] = accent_bright;
  c[ImGuiCol_TextSelectedBg] = with_alpha(accent, 0.42f);
  c[ImGuiCol_TreeLines] = with_alpha(accent, 0.30f);
  c[ImGuiCol_DragDropTarget] = accent_bright;
  c[ImGuiCol_DragDropTargetBg] = with_alpha(accent, 0.25f);
  c[ImGuiCol_UnsavedMarker] = accent_bright;
  c[ImGuiCol_NavCursor] = with_alpha(accent, 0.85f);
  c[ImGuiCol_NavWindowingHighlight] = with_alpha(accent_bright, 0.75f);
  c[ImGuiCol_NavWindowingDimBg] = ImVec4{0.031f, 0.024f, 0.055f, 0.60f};
  c[ImGuiCol_ModalWindowDimBg] = ImVec4{0.031f, 0.024f, 0.055f, 0.60f};

  // Metrics: generous rounding and a hairline border on every surface, which is
  // what makes the frosted panels read as separate sheets of glass.
  style.WindowRounding = 9.0f;
  style.ChildRounding = 7.0f;
  style.FrameRounding = 5.0f;
  style.PopupRounding = 7.0f;
  style.ScrollbarRounding = 7.0f;
  style.GrabRounding = 5.0f;
  style.TabRounding = 7.0f;
  style.WindowBorderSize = 1.0f;
  style.ChildBorderSize = 1.0f;
  style.PopupBorderSize = 1.0f;
  style.FrameBorderSize = 1.0f;
  style.TabBarBorderSize = 1.0f;
  style.TabBarOverlineSize = 2.0f;
  style.SeparatorTextBorderSize = 2.0f;
  style.DockingSeparatorSize = 2.0f;
  style.WindowPadding = {12.0f, 10.0f};
  style.FramePadding = {9.0f, 4.0f};
  style.ItemSpacing = {9.0f, 7.0f};
  style.ItemInnerSpacing = {8.0f, 5.0f};
  style.CellPadding = {8.0f, 4.0f};
  style.SeparatorTextPadding = {18.0f, 5.0f};
  style.IndentSpacing = 20.0f;
  style.ScrollbarSize = 12.0f;
  style.GrabMinSize = 11.0f;
  style.WindowTitleAlign = {0.0f, 0.5f};
  style.WindowMenuButtonPosition = ImGuiDir_Right;
  style.SeparatorTextAlign = {0.0f, 0.5f};
}

inline void ApplyRxImGuiStyle() { ApplyRxImGuiStyle(ImGui::GetStyle()); }

// Installs `ttf` (kRxDefaultFontPath's bytes, read through the Vfs by the
// caller) as imgui's default font. Copies into imgui's allocator because since
// 1.92 the atlas bakes glyphs lazily per size and needs the file for as long as
// it lives. Null `ttf` leaves whatever font is already there, so an app without
// the engine archive still gets a usable overlay.
inline ImFont* LoadRxImGuiFont(const void* ttf, size_t size,
                               f32 size_pixels = kRxDefaultFontSize) {
  if (ttf == nullptr || size == 0) return nullptr;
  void* owned = IM_ALLOC(size);
  std::memcpy(owned, ttf, size);
  return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(owned, static_cast<int>(size), size_pixels);
}

}  // namespace rx::render

#endif  // RX_RENDER_UTIL_IMGUI_THEME_H_
