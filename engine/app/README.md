# app

What this component hides: how the engine's subsystems are constructed,
ordered, stepped and torn down. `app::Host` is the composition root: it owns
the window, job system, world clock, ECS world/scheduler, renderer, physics,
vfs and audio, resolves the render quality preset, runs the fixed-step
simulation loop and assembles the per-frame `render::FrameView` (transform
gather + motion-vector history).

Game policy enters only through the `app::Application` callbacks
(`OnInitialize` / `OnFixedStep` / `OnUpdate` / `OnBuildView` / `OnFrameEnd` /
`OnShutdown`): a game implements that interface, hands it to a `Host`, and
never forks the loop. `runtime/` (the rx viewer) is the reference consumer.

Nothing below this layer links back to it; `app` is the only module allowed to
know about every subsystem.

Content and platform config: the host mounts the install layout (`rxe://` for
the engine, `<title>://` for the game, `AppConfig::title`) and resolves the
quality tier's platform config from `rxe://config/` and `<title>://config/`
(render settings, memory plan, options). See [docs/CONFIG.md](../../docs/CONFIG.md).

On a Steam Deck (`core/platform.h`, `RX_STEAMDECK=0/1` to override) the host
opens fullscreen (its memory plan comes from `rxe://config/steamdeck.ini`); under
any gamescope session it opens fullscreen too (`RX_FULLSCREEN` overrides).
`RX_FRAME_STATS=<seconds>` logs fps and p99/max frame time per window. While the
window is unfocused (the Steam menu on a Deck, alt-tab on a desktop) the host
caps the frame rate at `RX_UNFOCUSED_FPS` (default 10, 0 disables).
