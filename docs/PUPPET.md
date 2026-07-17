# Locomotion puppet (`--demo puppet`)

A graybox proving ground for the `rx::locomotion` module (see
[LOCOMOTION.md](LOCOMOTION.md)): a single physics-first ragdoll biped standing
on a flat floor with a shallow ramp, a curb and a few scattered boxes. It is the
runtime validation + visualization environment for the feedback controller — you
watch the 13-body ragdoll balance, step, walk, fall and ground itself while the
full `DebugState` is drawn on top as a line overlay.

Everything lives in `runtime/demo_puppet.{h,cc}`. It touches no controller
internals: it drives `rx::locomotion::LocomotionController` through its public
API (`Initialize` / `Tick` / `mode()` / `measurements()` / `contacts()` /
`debug()` / `rig()`) exactly as a game would. The registry wiring is the usual
few lines — a `puppet_` member on `DemoScenes`, a dispatch arm in
`CreateDemoScene`, a `puppet_->Emit(...)` call in `EmitToView`, and one
non-early-return route in `Viewer::OnUpdate` that forwards raw number keys (the
puppet keeps the normal free-fly camera).

Run it on a real GPU:

```
DISPLAY=:10 vkrun ./build/linux/runtime/rx --demo puppet
```

## Fixed-step cadence (no double-stepping)

The controller must `Tick` once per fixed physics step **before** the world is
integrated. The demo registers its tick in the ECS `kPreSim` stage; the host's
own `physics` system integrates Jolt in `kSim` right after. So the puppet drives
the controller and the host steps the shared `PhysicsWorld` exactly once each —
the demo never calls `PhysicsWorld::Update` itself. The default fixed step is
`1/60 s`; `RX_FIXED_DT=<seconds>` (a host-wide knob) makes it finer, which the
stiff joint motors track a little better.

## The scripted intent (hands-free)

With no input the puppet runs a looping 20 s script so an unattended capture
still shows locomotion:

| Phase | Duration | Intent |
| --- | --- | --- |
| Stand | 3 s | zero velocity, face −Z |
| Walk forward | 8 s | 0.5 m/s along −Z |
| Turn & walk | 6 s | heading eased −Z → −X over 3 s, 0.5 m/s along it |
| Stop | 3 s | zero velocity, face −X |

0.5 m/s is the portable stable-walking regime for this ragdoll (LOCOMOTION.md's
walk test documents that higher commands out-run the step controller). At the
end of the loop the puppet is **reset** (`Destroy` + `Initialize` at spawn) so it
recentres cleanly rather than teleporting — which also demonstrates the reset
path every cycle.

## Keys

The number keys are optional (the script is primary); they are edge-triggered
and take effect on the next fixed tick, before the physics step.

| Key | Action |
| --- | --- |
| **1** | Small push — a 40 kg·m/s impulse to the torso from a random horizontal direction (the controller should take a corrective step and re-settle). |
| **2** | Big push — a 200 kg·m/s impulse; enough to force `kControlledFall` → `kGrounded` (→ `kRecovering`). |
| **3** | Reset — destroy and re-initialize the puppet at spawn, restarting the script. |

WASD / mouse fly the normal free camera as in every other demo.

## The debug overlay

Drawn every frame from `controller.debug()` (`render::DebugLine`, the same path
as the nav overlay):

| Element | Meaning |
| --- | --- |
| **Capture-point cross + mast** | The linear-inverted-pendulum capture point, coloured **by control mode**: green `kStable`, yellow `kCorrectiveStep`, red `kControlledFall`, purple `kGrounded`, blue `kRecovering`. |
| **Cyan ring + cross** | The support centre and region under the supporting feet (drawn only while at least one foot supports). |
| **Foot-to-foot line** | Joins the two measured sole positions. |
| **Yellow cross + white line (per foot)** | The planned landing target and the line from the current sole to it. |
| **Magenta arc (per foot)** | The 8-segment swing trajectory (parabolic lift = `step_height`), drawn only while that foot is swinging. |
| **Green arrow / blue arrow from COM** | Desired vs measured planar velocity (scaled). |
| **Grey vertical line** | COM drop to the ground directly below it. |

Mode transitions are also logged once each with `RX_INFO` (`puppet demo: mode ->
…`), so a headless/log-only run is diagnosable without the overlay.

## The rig proxies

The 13 rig bodies each render as a box proxy posed from
`PhysicsWorld::GetBodyTransform` — sized per body part from the rig's public
geometry (limb boxes along the local capsule axis, the pelvis laterally, the feet
matching the foot box). The pelvis/torso are tinted warm, the head tan and the
limbs steel-blue. These are graybox proxies for readability, not the exact
collision capsules.

## Current capabilities (honest)

- **Stands** and holds balance with occasional corrective steps.
- **Push recovery**: shrugs off a small (40) torso impulse back to `kStable`; a
  big (200) impulse drives a **controlled fall** that grounds cleanly and then
  attempts a procedural get-up.
- **Slow walking** (~0.5 m/s command, tracking a fraction of it) as genuine
  physics-driven stepping, not an animation.
- Fast walking / running and robust turning-while-walking are **future work** —
  higher speed commands out-run the step controller and tip the trunk over the
  small feet, which the demo will honestly show as a `kControlledFall`. The
  overlay is built to make exactly that failure legible.

The controller module itself is under active development; this demo tracks
whatever the current controller does — every control mode above is reachable and
coloured, so it doubles as a live regression view.
