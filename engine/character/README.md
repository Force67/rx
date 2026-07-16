# rx::character

Engine-level, ECS-driven, functional-first player-character support: a capsule
**locomotion controller** (walk / run / sprint / crouch / jump, obstacle and
furniture collision, extensible stances) and **first / third-person view modes**
with real camera-collision management, composed on top of `rx::scene`'s camera
rig and driven by `rx::physics`' Jolt `CharacterVirtual`.

Everything here is generic engine code. Components are plain data; every system
is a free function. There are no manager classes, no singletons, and no
game-specific naming or behaviour — a title layers its own policy (input mapping,
stance rules, animation) on top.

## Coordinate convention

Shared with `rx::scene`: right-handed, +Y up, look yaw `0` faces `-Z`, and yaw
increases the same way `scene::CameraOrbit`'s does so a character heading and its
camera agree. Horizontal move input is **world-space** — the game rotates stick
input into world before filling `CharacterIntent`, keeping the controller free of
any camera/heading policy.

## Components (data)

| Component | Role |
| --- | --- |
| `CharacterMovementSettings` | Per-gait speeds (walk/run/sprint/crouch), ground accel/decel, air-control factor, jump height (impulse derived from `gravity`), step height, max slope angle. |
| `CharacterShape` | Standing + crouched capsule radius/height (heights are total tip-to-tip), standing + crouched eye height (from the feet), crouch blend speed. |
| `CharacterIntent` | Filled by the game each fixed step: world-space `move` (direction + `[0..1]` throttle magnitude), `gait`, `crouch` desired stance, `jump` edge, `look_yaw_delta` / `look_pitch_delta`. Edge/delta fields are consumed by `StepCharacters`. |
| `CharacterState` | Output: `stance` (Standing/Crouching; Swimming/Flying/Prone reserved), `grounded`, full `velocity`, `crouch_blend` `[0..1]`, current `eye_height`, `time_since_grounded` (coyote-friendly), heading `yaw`, `teleported` flag. |
| `CharacterBody` | Physics `CharacterId` + the live capsule dims (updated as the crouch blend resizes it). |
| `CharacterViewMode` | `FirstPerson` / `ThirdPerson`; view helpers switch this and re-compose the rig components. |

## Systems (free functions)

- `StepCharacters(world, physics, dt)` — consumes `CharacterIntent`: updates
  heading yaw and forwards pitch to a co-located `scene::CameraIntent`;
  accelerates velocity toward the gait target (ground accel vs air control);
  integrates gravity and jump; resolves stance (crouch enter is free, crouch
  **exit requires headroom** via an upward sphere cast); resizes the capsule
  **feet-planted** across the crouch blend; drives the physics controller with
  `MoveCharacterVelocity`; writes back `scene::Transform` + `CharacterState`.
  Obstacle / furniture / wall collision and step-up come from Jolt
  `CharacterVirtual` itself (step height + slope are pushed via
  `PhysicsWorld::ConfigureCharacter`).
- `SyncCharacterCameraAnchors(world)` — for entities with both `CharacterState`
  and `scene::CameraAnchor`, writes the anchor (feet + eye-height position,
  heading orientation, velocity) and bumps `revision` once after a teleport.
- `AnswerCameraObstructions(world, physics)` — the physics bridge that makes
  third-person camera collision real: for each `scene::CameraObstruction` with a
  fresh `request_id`, sphere-casts origin→desired and answers with
  `scene::SetCameraObstructionResult` (safe position pulled in front by `margin`).

### View-mode helpers (compose the existing rig components)

- `ApplyCharacterViewMode(world, entity, settings)` — installs/rewrites the rig
  for the entity's `CharacterViewMode.kind`. **First person**: eye-anchored
  `CameraOrbit` (yaw from the anchor, pitch clamped ~±85°), no boom, no lag.
  **Third person**: `CameraBoom` (distance + shoulder + height) + `CameraObstruction`
  + `CameraDamping`. Structural — call outside `World::Each`.
- `ToggleCharacterViewMode(world, entity, output, mode, settings, transition)` —
  flips the kind, re-applies the recipe, and drives a smooth FP↔TP transition
  through the camera-stack machinery (`PushCameraMode`, retiring the previous
  toggle's entry so the stack stays bounded).
- `ApplyCharacterZoom(world, entity, zoom_delta, allow_mode_switch, settings)` —
  optional: folds scroll into the third-person boom distance within `[min,max]`;
  with `allow_mode_switch`, zooming past the minimum switches to first person and
  zooming out of first person restores third person. Returns whether the kind
  changed.
- `TeleportCharacter(world, physics, entity, feet_position)` — snaps the capsule,
  resets velocity, and requests a one-shot anchor revision bump so the camera
  cuts instead of interpolating across the jump.

## Per-fixed-step call order

A consuming game runs, once per fixed step:

1. **Fill intent** — write each player's `CharacterIntent` (and any non-character
   `scene::CameraIntent` deltas).
2. `character::StepCharacters(world, physics, dt)`
3. `character::SyncCharacterCameraAnchors(world)`
4. `scene::BuildCameraRigs(world, dt)`
5. `scene::PrepareCameraRigConstraints(world, dt)`
6. `character::AnswerCameraObstructions(world, physics)`
7. `scene::ResolveCameraRigs(world, dt)`
8. `scene::ResolveCameraStacks(world, dt)`

`physics.Update(dt)` is stepped as usual (order relative to `StepCharacters` is
the game's call; the controller integrates its own gravity via
`MoveCharacterVelocity`). View-mode toggles/zoom are called from input handling,
outside `World::Each`.

## PhysicsWorld additions (this wave)

Added to `rx::physics` (real + stub):

- `GetCharacterPosition(id, out)` — capsule-centre of a live controller.
- `ConfigureCharacter(id, max_slope_angle, step_height)` — stair/slope tuning
  honoured by the `Move*` calls.
- `SetCharacterShape(id, radius, half_height)` — in-place capsule swap with a
  penetration test; returns `false` (keeping the old shape) when blocked.
- `SphereCast(origin, direction, max_distance, radius, out)` — swept-sphere
  closest hit, used for camera collision and the uncrouch headroom probe.

## Defaults chosen (for the next-wave "gym" scale-tuning app)

- Speeds: walk `1.6`, run `4.2`, sprint `6.5`, crouch `1.5` m/s.
- Accel/decel `45` / `55` m/s², air control `0.35`, gravity `9.81`.
- Jump height `1.1 m`, step height `0.4 m`, max slope `~55°`.
- Standing capsule `0.3 r × 1.8 h`, eye `1.62 m`; crouched `0.3 r × 1.2 h`,
  eye `0.95 m`; crouch blend `9 /s`.
- Third-person boom `3.25 m` (min `1.4`, max `6.0`), shoulder `0.45`,
  obstruction radius `0.25`, margin `0.12`.
