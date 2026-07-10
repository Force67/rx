# app

What this component hides: how the engine's subsystems are constructed,
ordered, stepped and torn down. `app::Host` is the composition root - it owns
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
