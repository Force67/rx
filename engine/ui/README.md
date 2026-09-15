# ui

What this component hides: that the engine has a retained-mode UI at all.
libultragui runs here in draw-data mode — it owns no window, no device and no
event loop; rx feeds it a `UguiHostState` viewport, pushes input into its queue
and records its draw list through `GuiRenderBackend`, a Vulkan backend on the
engine's own dynamic-rendering path (`FrameView::hud_draw`).

`ui::Splash` is the engine's own consumer: the rx plate `app::Host` puts up over
the first seconds of every windowed run, with the wordmark embedded in the
binary (`brand/`). It keeps a private `ugui::UIContext` and parks it, so an
application's own ultragui UI keeps the thread's active widget registry the
whole time the plate is up.

`ugui_platform.cc` and `ugui_rhi.cc` are compiled *into* libultragui in place of
its bundled GLFW/Vulkan backends, not into `rx_ui` — that is why they include
their headers relative instead of through the engine include root.
