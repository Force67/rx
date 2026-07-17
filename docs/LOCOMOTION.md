# rx::locomotion — physics-first articulated locomotion

`engine/locomotion` makes a jointed ragdoll biped stand, walk, take corrective
steps, and fall — as a **feedback controller over the physics simulation**, not
as an animation player. Every fixed step the controller measures the simulated
body, plans support contacts, generates continuous numeric targets, and drives
the joint motors toward them:

    typed movement intent
        -> measured physical state
        -> contact and balance plan
        -> continuous body targets
        -> joint motor commands
        -> physics simulation
        -> newly measured physical state

There are **no animation clips, no motion-block library, no style/archetype
hierarchy, no fallback inheritance, and no blackboard**. Movement variety comes
from continuous numeric parameters (speeds, gains, step geometry) in one flat
`ControllerParameters` object per character, resolved completely at spawn.

## Design provenance

The controller is independently designed from broadly published character
control concepts: proportional-derivative joint control, analytic inverse
kinematics, centre-of-mass / support-region balance, the linear inverted
pendulum capture-point approximation, procedural gait phase, footstep planning,
and published physics-based locomotion research (SIMBICON-family controllers,
example-guided controllers such as DeepMimic). No proprietary source material,
extracted game data, or reverse-engineered formats were used or referenced.

As a deliberate architectural constraint, the module must never grow:

- a class of attributed animation building blocks selected by attribute search,
- a class grouping such blocks into named movement/style categories,
- a character archetype class encapsulating those categories,
- archetype fallback/inheritance that supplies missing movement categories,
- a generic shared key-value blackboard that selects movement variants,
- data-authored transition tables sequencing grouped animation blocks.

Reviewers should reject changes that reintroduce that shape under any naming.

## Coordinate convention

Shared with `rx::scene` / `rx::character`: right-handed, **+Y up**, facing yaw
`0` looks down **-Z**. All positions in metres, velocities in m/s, masses in kg,
angles in radians, torques in N·m. The controller runs **only** in the fixed
physics step, before `PhysicsWorld::Update`.

## Module map

| File | Role |
| --- | --- |
| `types.h` | All typed data: `LocomotionIntent`, `PhysicalModifiers`, flat `ControllerParameters`, `CharacterMeasurements`, `ContactEstimate`, `GaitState`, `FootPlan`, `WholeBodyTargets`, `ControlMode`, `DebugState`. |
| `rig.h/.cc` | `BipedRig`: builds a 13-body / 12-joint ragdoll from `ControllerParameters` via `physics::PhysicsWorld` (capsule shapes, swing-twist hips/shoulders/waist/neck/ankles, hinge knees/elbows), snapshots bind constraint orientations, converts joint-local target rotations into constraint-space motor targets. |
| `gait.h/.cc` | `GaitClock`: continuous phase in `[0,1)`, opposite feet at `phase` / `phase+0.5`, speed-dependent phase rate and stance fraction. Start/stop logic. |
| `footstep.h/.cc` | Footstep planner: next-step target from desired velocity, lateral stance offset, velocity error and capture-point correction; terrain projection through a caller-supplied probe; clamped step geometry; analytic swing trajectory (smoothstep horizontal + parabolic lift). |
| `estimator.h/.cc` | `StateEstimator` (mass-weighted COM and COM velocity, ground normal, body height) and `ContactEstimator` (per-foot supporting / sliding / swinging / unconfirmed with hysteresis). |
| `whole_body.h/.cc` | Whole-body target generation: pelvis height/velocity targets, torso orientation from facing + ground normal + acceleration lean, analytic 2-bone leg IK, gait-phase arm counter-swing; outputs joint-local target rotations plus per-group drive scales. |
| `controller.cc` | `LocomotionController`: the fixed-update orchestration, balance assist, control-mode machine (`Stable`, `CorrectiveStep`, `ControlledFall`, `Grounded`, `Recovering`), controlled fall, grounded detection, procedural recovery. |

## Control modes

`ControlMode` is a small physical-regime enum; it owns no resources:

- `kStable` — nominal standing/walking.
- `kCorrectiveStep` — the capture point left the support margin but a reachable
  step can catch it; the next swing target is retargeted.
- `kControlledFall` — recovery is implausible; stance stiffness blends down,
  the arms loosen, extreme angular velocity is damped. Motors are never cut
  instantly.
- `kGrounded` — sustained torso/pelvis environment contact with low body
  velocity for a dwell time.
- `kRecovering` — procedural get-up: establish hand/knee support, move the COM
  over the support region, raise the pelvis, stand.

## Testing

- `test/locomotion_math_test.cc` — pure math (gait clock, capture point, step
  clamping, swing trajectory, leg IK), no Jolt required.
- `test/locomotion_test.cc` — Jolt acceptance tests: rig build sanity, motor
  target convergence, standing survival + push recovery, walking speed
  tracking, unrecoverable-push fall, no NaNs anywhere.

## Debugging

Every tick fills `DebugState` (COM, capture point, support region, planned foot
targets, swing trajectories, joint torque saturation, control mode, rejected
step reason) so instability is diagnosable from recorded numeric state alone.
The runtime `--demo puppet` scene draws it.
