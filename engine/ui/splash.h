#ifndef RX_UI_SPLASH_H_
#define RX_UI_SPLASH_H_

#include <optional>

#include <ugui/ui_context.h>

#include "asset/vfs.h"
#include "core/export.h"
#include "core/types.h"
#include "core/window.h"
#include "render/core/renderer.h"
#include "ui/ugui_backend.h"
#include "ui/ugui_platform.h"

namespace rx::ui {

// The engine plate every rx application shows over its first frames: the rx
// wordmark on a light field, held for a couple of seconds and then faded out.
// app::Host owns one and drives it; an application never sees it.
//
// It draws through FrameView::hud_draw, the same slot an application's own
// ultragui HUD uses, and the host installs it *after* OnBuildView. So while the
// splash is up it takes that slot over: for its ~2.4s a game's own HUD does not
// record. That is the point (the plate is opaque and full-screen, so anything
// underneath is invisible anyway) but it is why the host drops the splash the
// moment it is done rather than keeping an idle instance around.
//
// Nothing here is conditional on content loading. The plate is a fixed-length
// title card, not a progress bar: an application that boots in 40ms still shows
// it, and one that takes ten seconds shows a frozen last splash frame only if
// it blocks the main thread, which is its own bug.
class RX_UI_EXPORT Splash {
 public:
  // How long the plate is up in total, and how much of that tail is the fade to
  // the application's first visible frame. RX_SPLASH_SECONDS overrides the
  // former; the fade is a constant so a shortened splash still lands softly.
  static constexpr f32 kDefaultSeconds = 2.4f;
  static constexpr f32 kFadeOutSeconds = 0.45f;

  Splash() = default;
  ~Splash();

  Splash(const Splash&) = delete;
  Splash& operator=(const Splash&) = delete;

  // Brings up an ultragui context in draw-data mode, rasterizes the embedded
  // wordmark and lays the plate out. Returns false when the splash cannot draw
  // (no Vulkan backend, ultragui refused to initialize); the host then simply
  // runs without one. `vfs` supplies the text font through fonts://.
  bool Initialize(Window& window, render::Renderer& renderer, asset::Vfs& vfs,
                  f32 seconds = kDefaultSeconds);

  // Advances the timeline by one frame. Returns false once the plate is spent
  // and the host should drop it.
  bool Update(f32 frame_delta);

  // Takes FrameView::hud_draw for this frame. Call after the application built
  // its view, so the plate covers whatever the application drew.
  void Draw(render::FrameView& view);

  void Shutdown();

 private:
  // The logo's draw box, in the logical units the document is written in.
  struct LogoBox {
    f32 width = 0;
    f32 height = 0;
  };

  // Rasterizes the embedded wordmark at the size it will be drawn, trims its
  // transparent margin and uploads it. Sets logo_aspect_.
  bool LoadLogo();
  // Fits a logo of `aspect` into the window. Both LoadLogo (to pick a
  // rasterization size) and BuildDocument (to write the box out) need it, and
  // they have to agree or the wordmark is resampled for nothing.
  LogoBox MeasureLogoBox(f32 aspect) const;
  // Resolves a font file ultragui can open. rx ships Roboto inside rx_fonts.rxp
  // and ultragui's TextEngine only takes a path, so the bytes are materialized
  // into a cache file once; the system-font fallback covers an unpacked tree.
  bool LoadFont(asset::Vfs& vfs);
  // Rebuilds the document. Called once per layout-affecting change: startup and
  // a window resize.
  void BuildDocument();

  render::Renderer* renderer_ = nullptr;
  Window* window_ = nullptr;

  // Held by value only once Initialize ran: constructing a UIContext makes it
  // the thread's active WidgetRegistry, and `park_` hands that slot straight
  // back to whatever held it (see the note in Initialize). Declared in this
  // order so park_ unwinds before the context that displaced it.
  std::optional<ugui::UIContext> ui_;
  std::optional<ugui::WidgetRegistry::ScopedActive> park_;
  GuiRenderBackend backend_;
  UguiHostState host_state_;
  const ugui::DrawData* draw_data_ = nullptr;
  u32 font_revision_ = 0;

  ugui::TextureId logo_ = ugui::kNullTextureId;
  // Of the uploaded texture, which is the trimmed ink and not the artwork's
  // canvas. BuildDocument measures the draw box from it, so the two cannot
  // drift apart without the wordmark stretching to cover the difference.
  f32 logo_aspect_ = 1.0f;

  f32 total_seconds_ = kDefaultSeconds;
  f32 elapsed_ = 0.0f;
  f32 opacity_ = 1.0f;
  u32 laid_out_w_ = 0;
  u32 laid_out_h_ = 0;
  bool ready_ = false;
};

}  // namespace rx::ui

#endif  // RX_UI_SPLASH_H_
