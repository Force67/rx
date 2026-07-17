# Character gym (`--demo gym`)

A graybox reference "gym" for the `rx::character` and `rx::inventory` modules: a
dev-checker playground where a person walks around in first / third person and
tunes eye heights, capsule dims and player scale against **known-size geometry**.
It is the validation and tuning environment for the character controller and a
light showcase of the inventory module.

Everything lives in `runtime/demo_gym.{h,cc}`. It touches no engine internals —
it drives the two modules and the scene camera rig through their public APIs
exactly as a game would. The registry wiring is the usual few lines: a `gym_`
member on `DemoScenes`, a dispatch arm in `CreateDemoScene`, a `gym_->Emit(...)`
call in `EmitToView`, and (because the gym owns its camera + input) a
`gym()->Update(...)` route in `Viewer::OnUpdate`.

Run it on a real GPU:

```
DISPLAY=:10 vkrun ./build/linux/runtime/rx --demo gym
```

## Controls

| Input | Action |
| --- | --- |
| **WASD** / left stick | Move (camera-relative) |
| **Mouse** / right stick | Look |
| **Shift** | Sprint (hold) |
| **Ctrl** | Crouch (hold) |
| **Space** | Jump — or, with the jetpack **on**, hold to thrust (the normal jump is suppressed while the pack is on) |
| **J** | Toggle the jetpack on / off |
| **V** | Toggle first / third person (smooth stack transition) |
| **Scroll** | Zoom the third-person boom (`ApplyCharacterZoom`) |
| **G** | Drop a crate from the inventory (forward impulse) |
| **F** / **T** | Pick up the nearest dropped crate |
| **R** | Reset the player to spawn |
| **Tab** | Release / recapture the cursor so the tuning panel is clickable |
| **F1** | Toggle the renderer debug overlay |

## The tuning panel (the point of the gym)

An imgui panel (top-right) with a live readout — view mode, stance, grounded,
speed, eye height, crouch %, capsule dims, inventory count — and sliders that
apply to the **live** components every frame:

- **Eye height:** standing / crouched eye height.
- **Capsule:** standing + crouched radius / height, crouch blend speed. Changes
  flow through `StepCharacters`' feet-planted resize path (`SetCharacterShape`).
- **Speeds & jump:** per-gait speeds (walk / run / sprint / crouch), jump height,
  step height, max slope (deg; re-pushed to the controller via
  `ConfigureCharacter`), gravity.
- **Feel (never robotic):** turn / pivot half-life + pivot angle (body-facing turn
  smoothing), gait speed-blend + stop epsilon + air control, jump buffer + coyote
  time, eye step-smoothing half-life and landing-dip scale / cap / recovery. The
  top readout shows live `look` vs `facing` yaw (with a `(pivot)` marker), the
  blended `gait tgt` speed, and the current buffer / coyote / dip values.
- **Third-person camera:** boom distance, shoulder / height offsets, min / max
  distance, obstruction radius — written straight onto the rig's `CameraBoom` /
  `CameraObstruction` / `CameraDamping` / `CameraOrbit` components.
- **Look:** mouse sensitivity, invert pitch.

## Calibration content (all sizes exact, in metres)

| Content | Purpose |
| --- | --- |
| **Reference cubes** 0.25 / 0.5 / 1.0 / 2.0 m | Scale reference; the 2 m cube is "door height", human-scale. |
| **Doorway** 1.0 × 2.1 m clear | Eye-height / clearance framing. |
| **Staircases** 0.15 m and 0.30 m risers | Step-up vs blocked-step tuning. |
| **Ramps** 30° / 45° / 60° | Slope walkability — the 60° ramp is above the ~55° limit and is deliberately **unwalkable**. |
| **Crouch tunnel** 1.25 m clearance | Forces crouch; the 1.8 m standing capsule cannot uncrouch under the roof (headroom-checked). |
| **Furniture** 0.75 m table, 0.45 m seat, 1.0 m counter | "Furniture in the way" collision. |
| **Narrow gap** 0.7 m | Squeeze test between two walls. |
| **Moving platform** | Kinematic `AddKinematicBox` + `MoveBodyKinematic` demo (character platform-riding is not yet folded into the module, so this is a demonstration, not a validated ride). |
| **Ground** | Checker at 1 m grid so distances read at a glance. |

The **reference-cube texture** is a procedural classic dev-checker: a mid-gray
two-tone at ~8×8 cells per metre with a contrasting orange orientation corner /
arrow, generated at startup. UVs are baked in world metres so the checker is
continuous across every object and the cell size reads as a constant real-world
scale. Object classes are tinted variants of the one texture (neutral cubes, warm
furniture, cool structure, green crates, blue player proxy). Lighting is a fixed
bright noon sun; lens flare and RT reflections/SSR are disabled so the flat
graybox surfaces stay readable.

## The player

An entity wired exactly per the `rx::character` README — capsule + movement +
shape + intent + state + view mode + camera rig — plus a visible **capsule proxy
mesh** (a checker-tinted cylinder + two sphere caps, sized from the live capsule
so it shrinks when crouched) drawn only in third person. The per-fixed-step call
order (fill intent → `StepCharacters` → `SyncCharacterCameraAnchors` →
`BuildCameraRigs` → `PrepareCameraRigConstraints` → `AnswerCameraObstructions` →
`ResolveCameraRigs` → `ResolveCameraStacks`) runs in `GymDemo::Update`; the
resolved `CameraOutput` is read back into the frame view each frame.

## Inventory garnish

One `ItemDef` — a 0.25 m checker-cube "crate" — is registered in a gym-owned
`ItemCatalog`; the player spawns with 8. **G** drops one as a dynamic body via
`inventory_world::DropItem` (forward impulse); **F** / **T** pick up the nearest
`WorldItem` via `PickUpItem`. `SyncWorldItems` / `HibernateDistantWorldItems` /
`WakeWorldItemsNear` run each frame in the README's per-tick order. The panel
shows the live crate count.

## Jetpack

`engine/character/jetpack.{h,cc}` in the module's idiom: plain-data components
(**`JetpackDesc`**, **`JetpackInput`**, **`JetpackState`**) plus one
free-function system, **`StepJetpacks(world, dt)`**, staged **before**
`StepCharacters` each fixed step. **J** toggles the pack; with it on, hold
**Space** to burn.

The controller seam is the interesting part. The character controller is a
kinematic, velocity-based mover with no mass to push, so the pack works in
**acceleration** units and injects thrust through the new
**`CharacterIntent::external_acceleration`** field, which `StepCharacters` folds
into the velocity just before the mover consumes it. That lets thrust compete
with gravity honestly, and because the ground clamp still stops downward motion
the pack never bypasses the existing fall/land handling.

Fuel / spool / refuel model:

- Thrust lags demand through a first-order spool (**`spool_time`** ≈ 0.3 s ≈ 90%
  rise time), so a stab of the button does not snap to full thrust.
- Fuel drains proportional to the *actual* (spooled) thrust
  (**`fuel_capacity_s`** = 4 s of full burn); an empty tank is a dead stick —
  thrust forced to zero, normal gravity fall.
- Refuel **only while grounded** (**`refuel_rate`** = 0.5 tank/s → full in 2 s);
  airborne never refuels.
- **`thrust_to_weight`** = 1.45 — the pack climbs but does not hover on its own.
  There is **no auto-hover**: matching thrust to weight to hang still is the
  player's finesse (intended).
- **`lateral_accel`** = 24 m/s² of thrust-vector authority scaled by the actual
  thrust, applied along the move intent so airborne WASD accelerates faster with
  the pack burning than a free-fall drift.

**Measured behaviour** (from `test/jetpack_test.cc`, headless, over the real
Jolt character controller):

| Scenario | Result |
| --- | --- |
| Full burn from rest | Leaves the ground and climbs ~27 m over a 3 s burn. |
| Fuel drain | Empties in ~4.1 s (≈ capacity); thrust dies and the character descends. |
| Refuel | Grounded restores the tank (0.2 → 1.0); airborne never refuels (stays 0.5). |
| Spool lag | Thrust ~0.59 at `spool_time`/3, ~0.91 at `spool_time` (target 0.90). |
| Airborne lateral | WASD reaches ~6.5 m/s horizontal burning vs ~4.2 m/s free-fall drift. |
| Robustness | NaN-free under random input; the plain jump still works (~1.05 m) with the pack off. |

## Env-gated staging (for captures)

Small, clean hooks for deterministic headless-style captures (they double as a
smoke path). When `RX_GYM_SCRIPT` is set the gym advances on a fixed 1/60 step so
a capture at frame N is frame-rate independent, and the cursor is not grabbed.

| Env | Effect |
| --- | --- |
| `RX_GYM_SPAWN="x,z"` or `"x,z,y"` | Player spawn (feet), metres. |
| `RX_GYM_YAW=<radians>` | Initial heading (0 faces −Z). |
| `RX_GYM_PITCH=<radians>` | Initial camera pitch (negative looks down). |
| `RX_GYM_VIEW=fp\|tp` | Initial view mode. |
| `RX_GYM_SCRIPT="t:token,..."` | Timed intent: `fwd`/`fwdhalf` (analog half-stick)/`back`/`left`/`right`/`stop`, `sprint`/`run`, `crouch`/`stand`, `jump`, `view`, `drop`, `pickup`. |

Pair with the viewer's `RX_UI_SHOT=<path>` / `RX_UI_SHOT_FRAMES` and
`RX_WIN_W`/`RX_WIN_H` for a timed swapchain capture.
