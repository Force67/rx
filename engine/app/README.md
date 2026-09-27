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

Render settings resolve in this order: the quality tier (`AppConfig::preset`,
`kAuto` detects the gpu and recognizes a Steam Deck), the `RX_*` env
overrides, the project's overlays (`AppConfig::render_ini_dir`: `default.ini`,
then `<tier>.ini`), `RX_RENDER_INI`, and finally `AppConfig::tune_settings`.

On a Steam Deck (`core/platform.h`, `RX_STEAMDECK=0/1` to override) the host
opens fullscreen and the memory plan defaults to the `steamdeck` preset; under
any gamescope session it opens fullscreen too (`RX_FULLSCREEN` overrides).
