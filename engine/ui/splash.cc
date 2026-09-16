#include "ui/splash.h"

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <system_error>

#include <ugui/core/color.h>
#include <ugui/style/style.h>
#include <ugui/svg/svg.h>
#include <ugui/svg/svg_types.h>
#include <ugui/widgets/image.h>
#include <ugui/widgets/widget.h>

#include "core/log.h"
#include "render/rhi/vulkan_interop.h"
#include "ui/brand/rx_engine_svg.h"

// The rx splash plate: bring up a private ultragui context, rasterize the
// embedded wordmark, lay out the title card and fade it off the screen.
namespace rx::ui {
namespace {

namespace fs = std::filesystem;

// Fractions of the window the logo is allowed to take. Height leads (the plate
// reads as a band across the middle); the width clamp keeps the wordmark off
// the edges of a wide or ultrawide window.
constexpr f32 kLogoHeightFraction = 0.26f;
constexpr f32 kLogoWidthFraction = 0.52f;

// rx ships Roboto inside rx_fonts.rxp, so the face has no path of its own and
// is handed to ultragui as bytes (LoadFontMemory, which keeps its own copy).
constexpr const char* kFontAsset = "fonts://roboto/Roboto-Medium.ttf";

// A font on the machine, for a tree whose rx_fonts.rxp was never packed. The
// plate is the wordmark either way: losing this costs the one line above it,
// not the splash.
const char* SystemFont() {
  static const char* kCandidates[] = {
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/TTF/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
      "/run/current-system/sw/share/X11/fonts/DejaVuSans.ttf",
  };
  for (const char* candidate : kCandidates) {
    std::error_code ec;
    if (fs::exists(candidate, ec)) return candidate;
  }
  return nullptr;
}

// Crops fully transparent rows and columns off an RGBA8 image in place. The
// wordmark sits in a canvas with a wide margin; laying that margin out as if it
// were logo would push the line above it away by an amount that depends on the
// artwork rather than on the design.
void TrimTransparentBorder(ugui::SvgImage& image) {
  const u32 w = image.width;
  const u32 h = image.height;
  if (w == 0 || h == 0) return;
  auto alpha = [&](u32 x, u32 y) { return image.pixels[(y * w + x) * 4 + 3]; };

  u32 min_x = w, min_y = h, max_x = 0, max_y = 0;
  for (u32 y = 0; y < h; ++y) {
    for (u32 x = 0; x < w; ++x) {
      if (alpha(x, y) == 0) continue;
      if (x < min_x) min_x = x;
      if (x > max_x) max_x = x;
      if (y < min_y) min_y = y;
      if (y > max_y) max_y = y;
    }
  }
  if (min_x > max_x || min_y > max_y) return;  // fully transparent: leave it

  const u32 out_w = max_x - min_x + 1;
  const u32 out_h = max_y - min_y + 1;
  if (out_w == w && out_h == h) return;
  // Rows only ever move up and left, so a forward copy never reads a row it
  // has already overwritten.
  for (u32 y = 0; y < out_h; ++y) {
    const u8* src = &image.pixels[((y + min_y) * w + min_x) * 4];
    u8* dst = &image.pixels[y * out_w * 4];
    std::memmove(dst, src, static_cast<std::size_t>(out_w) * 4);
  }
  image.pixels.resize(static_cast<std::size_t>(out_w) * out_h * 4);
  image.width = out_w;
  image.height = out_h;
}

}  // namespace

Splash::~Splash() { Shutdown(); }

bool Splash::Initialize(Window& window, render::Renderer& renderer, asset::Vfs& vfs,
                        f32 seconds) {
  window_ = &window;
  renderer_ = &renderer;
  total_seconds_ = seconds;

  render::VulkanHandles vk = render::GetVulkanHandles(*renderer.device());
  if (!vk.device) {
    RX_INFO("splash: renderer is not on the vulkan backend, skipping the plate");
    return false;
  }

  host_state_.window_width = static_cast<f32>(window.width());
  host_state_.window_height = static_cast<f32>(window.height());
  host_state_.dpi_scale = window.pixel_density();

  // ugui::UIContext makes itself the thread's active WidgetRegistry for its
  // whole lifetime, and every free function (SetImageTexture, SetStyle, ...)
  // writes to whichever registry is active. An application with a ultragui UI
  // of its own already holds that slot, so the splash parks its context the
  // moment it has one and re-activates it only for the length of its own calls.
  // Without that, the editor would build its widgets into the splash's registry
  // for the two seconds the plate is up.
  ugui::WidgetRegistry* application_registry = ugui::WidgetRegistry::Active();
  ui_.emplace();
  park_.emplace(application_registry);
  ugui::WidgetRegistry::ScopedActive scope(&ui_->world());

  ugui::UIConfig cfg;
  cfg.draw_data = true;
  cfg.external_window = &host_state_;
  cfg.width = static_cast<i32>(window.width());
  cfg.height = static_cast<i32>(window.height());
  ui_->set_ui_scale(window.pixel_density());  // before Init: it scales too
  if (!ui_->Init(cfg)) {
    RX_WARN("splash: ultragui refused to initialize, skipping the plate");
    return false;
  }

  GuiRenderBackend::InitInfo info;
  info.instance = vk.instance;
  info.physical_device = vk.physical_device;
  info.device = vk.device;
  info.queue_family = vk.graphics_family;
  info.queue = vk.graphics_queue;
  info.color_format = render::GetVkFormat(renderer.swapchain_format());
  info.frames_in_flight = render::GetVulkanFramesInFlight(*renderer.device());
  if (!backend_.Init(info)) {
    RX_WARN("splash: ugui vulkan backend failed to initialize, skipping the plate");
    return false;
  }
  ui_->set_texture_backend(&backend_);

  LoadFont(vfs);
  if (!LoadLogo()) return false;

  BuildDocument();
  ready_ = true;
  return true;
}

bool Splash::LoadFont(asset::Vfs& vfs) {
  if (std::optional<base::Vector<u8>> bytes = vfs.Read(kFontAsset)) {
    const ugui::FontHandle font = ui_->LoadFontMemory(
        reinterpret_cast<const char*>(bytes->data()), bytes->size());
    if (font != ugui::kInvalidFont) {
      ui_->set_default_font(font);
      return true;
    }
  }
  if (const char* system = SystemFont()) {
    ui_->set_default_font(ui_->LoadFont(system));
    return true;
  }
  // The wordmark carries the plate on its own; only the line above it is lost.
  RX_WARN("splash: no font (mount rx_fonts.rxp); the plate draws without text");
  return false;
}

bool Splash::LoadLogo() {
  const char* svg = reinterpret_cast<const char*>(kRxEngineSvg);
  // LoadSvgMemory reads a zero dimension as "take the document's own", not as
  // "keep the aspect", so every call below passes both. Parsing is separate
  // from rasterizing, so this costs one pass over 20KB of text and no pixels.
  ugui::svg::Document document;
  if (!ugui::svg::ParseSvg(svg, sizeof(kRxEngineSvg), document) || document.height <= 0.0f) {
    RX_ERROR("splash: cannot parse the embedded rx wordmark");
    return false;
  }
  const f32 canvas_aspect = document.width / document.height;

  auto rasterize = [&](u32 canvas_height, ugui::SvgImage& out) {
    const u32 canvas_width =
        static_cast<u32>(canvas_aspect * static_cast<f32>(canvas_height) + 0.5f);
    if (!ugui::LoadSvgMemory(svg, sizeof(kRxEngineSvg), out, canvas_width, canvas_height))
      return false;
    TrimTransparentBorder(out);
    return out.width > 0 && out.height > 0;
  };

  // Where the ink sits inside the canvas is a property of the artwork, and the
  // draw box follows from the ink's aspect, so it takes a cheap probe before
  // the size to rasterize for real is even known. Rasterizing straight at the
  // draw box instead leaves the trimmed logo a third of that size, stretched
  // back up over the box: a visibly soft wordmark.
  constexpr u32 kProbeCanvasHeight = 256;
  ugui::SvgImage probe;
  if (!rasterize(kProbeCanvasHeight, probe)) {
    RX_ERROR("splash: cannot rasterize the embedded rx wordmark");
    return false;
  }
  const f32 ink_aspect = static_cast<f32>(probe.width) / static_cast<f32>(probe.height);
  const f32 ink_height_fraction =
      static_cast<f32>(probe.height) / static_cast<f32>(kProbeCanvasHeight);

  const LogoBox box = MeasureLogoBox(ink_aspect);
  ugui::SvgImage image;
  const u32 canvas_height =
      static_cast<u32>(box.height * window_->pixel_density() / ink_height_fraction + 0.5f);
  if (!rasterize(canvas_height, image)) {
    RX_ERROR("splash: cannot rasterize the embedded rx wordmark");
    return false;
  }

  // Each trim rounds its four edges on its own, so the final raster's aspect
  // sits near the probe's without matching it. The draw box follows the raster
  // this uploads. A box measured from the probe stretches the wordmark by the
  // difference between the two.
  logo_aspect_ = static_cast<f32>(image.width) / static_cast<f32>(image.height);

  logo_ = backend_.CreateTexture(image.width, image.height, ugui::RHIFormat::kRgba8Unorm,
                                 image.pixels.data(), ugui::RHIFilter::kLinear);
  if (logo_ == ugui::kNullTextureId) {
    RX_ERROR("splash: cannot upload the rx wordmark");
    return false;
  }
  return true;
}

// The logo's draw box in logical units: ugui multiplies every px by the
// ui_scale taken from the window's pixel density, so dividing the framebuffer
// size out here keeps the plate the same physical size on every display.
Splash::LogoBox Splash::MeasureLogoBox(f32 aspect) const {
  const f32 density = window_->pixel_density() > 0.0f ? window_->pixel_density() : 1.0f;
  const f32 view_w = static_cast<f32>(window_->width()) / density;
  const f32 view_h = static_cast<f32>(window_->height()) / density;

  LogoBox box;
  box.height = view_h * kLogoHeightFraction;
  box.width = box.height * aspect;
  if (box.width > view_w * kLogoWidthFraction) {
    box.width = view_w * kLogoWidthFraction;
    box.height = aspect > 0.0f ? box.width / aspect : box.height;
  }
  return box;
}

void Splash::BuildDocument() {
  const LogoBox box = MeasureLogoBox(logo_aspect_);
  const f32 logo_w = box.width;
  const f32 logo_h = box.height;
  const f32 density = window_->pixel_density() > 0.0f ? window_->pixel_density() : 1.0f;
  // Overscan by kBleed on every side. A plate sized to exactly 100vw/100vh
  // leaves the outermost pixel row half-covered, and the scene bleeding through
  // that seam frames the whole splash in a faint outline. Symmetric, so the
  // centred column stays centred on the screen.
  constexpr f32 kBleed = 2.0f;
  const f32 plate_w = static_cast<f32>(window_->width()) / density + 2.0f * kBleed;
  const f32 plate_h = static_cast<f32>(window_->height()) / density + 2.0f * kBleed;

  char doc[1024];
  // ugui blits the texture into whatever rect layout hands the widget, so the
  // box has to hold the aspect of the upload. Fractional px, because rounding
  // the two sides apart skews it. flex-shrink: 0, because a column short on
  // room takes the height back and leaves the width alone.
  std::snprintf(doc, sizeof(doc),
                "panel splash_root {\n"
                "  width: %d; height: %d; margin: %d 0 0 %d;\n"
                "  background: #f4f4f5;\n"
                "  layout: column; justify: center; align: center;\n"
                "  text splash_made_with {\n"
                "    text: \"MADE WITH\"; font-size: 13; font-weight: medium;\n"
                "    letter-spacing: 7; color: #8a8f92; margin: 0 0 %d 0;\n"
                "  }\n"
                "  image splash_logo {\n"
                "    width: %.2f; height: %.2f; flex-shrink: 0;\n"
                "  }\n"
                "}\n",
                static_cast<int>(plate_w), static_cast<int>(plate_h),
                -static_cast<int>(kBleed), -static_cast<int>(kBleed),
                static_cast<int>(logo_h * 0.20f), logo_w, logo_h);

  ui_->LoadUiString(doc, "rx_splash");
  if (ugui::wid logo = ui_->FindWidget("splash_logo"); logo.valid())
    ugui::SetImageTexture(logo, logo_, logo_w, logo_h);

  laid_out_w_ = window_->width();
  laid_out_h_ = window_->height();
}

bool Splash::Update(f32 frame_delta) {
  if (!ready_) return false;
  elapsed_ += frame_delta;
  if (elapsed_ >= total_seconds_) return false;

  // Hold at full strength, then fade the whole plate out; opacity inherits
  // multiplicatively in ugui, so the root carries the wordmark and the line
  // above it.
  const f32 fade_starts = total_seconds_ - kFadeOutSeconds;
  opacity_ = elapsed_ <= fade_starts
                 ? 1.0f
                 : 1.0f - (elapsed_ - fade_starts) / kFadeOutSeconds;
  return true;
}

void Splash::Draw(render::FrameView& view) {
  if (!ready_) return;
  ugui::WidgetRegistry::ScopedActive scope(&ui_->world());

  host_state_.window_width = static_cast<f32>(window_->width());
  host_state_.window_height = static_cast<f32>(window_->height());
  host_state_.dpi_scale = window_->pixel_density();
  ui_->set_ui_scale(window_->pixel_density());
  if (window_->width() != laid_out_w_ || window_->height() != laid_out_h_)
    BuildDocument();  // a resize changes the logo box, so re-lay it out

  if (ugui::wid root = ui_->FindWidget("splash_root"); root.valid()) {
    if (ugui::StyleC* sc = ui_->world().Get<ugui::StyleC>(root)) {
      ugui::Style style = sc->style;
      style.opacity = opacity_;
      ugui::SetStyle(ui_->world(), root, style);
    }
  }

  draw_data_ = &ui_->RenderDrawData();
  if (ui_->text_engine().atlas_revision() != font_revision_) {
    ugui::Vec2 size = ui_->text_engine().atlas_size();
    backend_.UpdateFontAtlas(ui_->text_engine().atlas_pixels(), static_cast<u32>(size.x),
                             static_cast<u32>(size.y));
    font_revision_ = ui_->text_engine().atlas_revision();
  }
  backend_.NewFrame();
  // Takes the slot an application's own HUD would use. The host installs this
  // after OnBuildView precisely so the plate wins while it is up. The HUD
  // underneath still records: drop it and the fade dissolves to the bare scene
  // rather than to the application's first screen, which on a game is a world
  // that has not finished streaming.
  view.hud_draw = [this, under = std::move(view.hud_draw)](render::CommandList& cmd) {
    if (under) under(cmd);
    if (draw_data_) backend_.Render(*draw_data_, render::GetVkCommandBuffer(cmd));
  };
  // And drops the debug overlay, which records after hud_draw and would
  // otherwise be the one thing the plate does not cover. It comes back by
  // itself on the frame after the host lets the splash go.
  view.ui_draw = nullptr;
}

void Splash::Shutdown() {
  if (ui_) {
    {
      ugui::WidgetRegistry::ScopedActive scope(&ui_->world());
      if (logo_ != ugui::kNullTextureId) backend_.DestroyTexture(logo_);
      logo_ = ugui::kNullTextureId;
      backend_.Shutdown();
      ui_->Shutdown();
    }
    park_.reset();  // before ui_: ScopedActive restores in reverse order
    ui_.reset();
  }
  draw_data_ = nullptr;
  ready_ = false;
}

}  // namespace rx::ui
