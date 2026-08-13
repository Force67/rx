# Body dynamics and soft-tissue deformation

`rx::anim::BodyDynamics` is a per-character secondary-motion layer for soft
body regions. It runs after the base animation graph and IK, adds physically
damped motion to authored helper bones, and emits normalized deformation
signals for morph targets. It does not turn the render mesh into a general soft
body or put anatomy policy in the physics world.

The solver models every region as an analytically integrated second-order
system. Character acceleration, animation-derived bone acceleration, gravity,
turn acceleration, and explicit impact impulses contribute to the force. The
result is stable across variable frame rates and hitches, retains momentum
through animation changes, and is bounded by per-axis translation and rotation
limits. A teleport or large driver discontinuity resets history instead of
creating an explosive frame.

## Authoring a rig

Use one low-cost helper bone per independently moving mass and skin only the
affected area to it. The helper should be parented near the rigid anatomical
driver, with smooth weights crossing into rigid neighbors. Separate left and
right masses should be separate regions. A helper is optional: missing names
disable that region without affecting the rest of the rig.

The standard presets cover the soft areas where secondary motion is normally
visible:

| Preset | Character | Typical driver |
| --- | --- | --- |
| `kChest` | soft, underdamped | upper torso / chest |
| `kAbdomen` | slower, strongly damped | lower spine |
| `kGlutes` | moderate lag | pelvis / glutes |
| `kThigh` | firmer, smaller travel | corresponding thigh |
| `kUpperArm` | firm, restrained | corresponding upper arm |
| `kCalf` | very firm, minimal travel | corresponding calf |

Rigid joints, skull, hands, and feet intentionally have no preset. Add custom
regions only when an asset contains an appropriate deforming mass. Presets are
starting material values, not assumptions about sex, body shape, or skeleton
names. Body composition variants should tune frequency, damping, mobility, and
limits in asset metadata.

Corrective morph targets make the result substantially more realistic than
bones alone. Author separate stretch, compression, and lateral-shear shapes
with approximately volume-preserving bulges, then bind them through
`BodyMorphBinding`. An impact corrective can add a short compression wave.
`ApplyBodyMorphWeights` adds these outputs to existing clip and expression
weights, so facial animation and other morph producers remain intact.

## Runtime integration

Create one controller for each animated character and add the regions available
on that character's rig:

```cpp
anim::BodyDynamics body;

auto abdomen = anim::MakeBodyRegionPreset(
    anim::BodyRegionKind::kAbdomen, "abdomen", "SpineLower", "AbdomenSoft");
abdomen.morphs.push_back({"abdomenCompress",
                          anim::BodyDeformationSignal::kCompression, 0.8f});
abdomen.morphs.push_back(
    {"abdomenShear", anim::BodyDeformationSignal::kShear, 0.6f});
body.AddRegion(abdomen);
```

Each rendered/fixed animation update follows this order:

1. Evaluate the animation graph into `SkeletonPose`.
2. Apply whole-body/foot IK.
3. Fill `BodyDynamicsFrame` from resolved character motion. Acceleration is the
   change in collision-resolved velocity, not desired input acceleration.
4. Call `BodyDynamics::Update`.
5. Add returned body weights with `ApplyBodyMorphWeights`.
6. Build model matrices and the GPU skin palette.

```cpp
anim::BodyDynamicsFrame dynamics_frame;
dynamics_frame.linear_acceleration =
    (character.velocity - previous_velocity) / dt;
dynamics_frame.angular_acceleration =
    (body_angular_velocity - previous_angular_velocity) / dt;
dynamics_frame.gravity = character.grounded ? Vec3{0, -9.81f, 0} : Vec3{};
dynamics_frame.model_units_per_metre = 1.0f;
dynamics_frame.teleport = character.teleported;

base::Vector<anim::BodyMorphWeight> body_weights;
body.Update(skeleton, dynamics_frame, dt, &pose, &body_weights);
anim::ApplyBodyMorphWeights(mesh, body_weights, &dense_morph_weights);
```

Use `linear_impulse` and `angular_impulse` for discrete landings or collisions;
do not repeat them on subsequent frames. Feeding resolved acceleration matters:
it naturally produces landing compression, wall-stop rebound, and the absence
of fake jiggle when movement input is blocked.

`model_units_per_metre` keeps physical tuning portable. Set it to the number of
skeleton-space units in one metre for source-native rigs; all region limits and
physics values remain SI units.

## Tuning and performance

- Start with a preset, then tune frequency before damping. Lower frequency is a
  softer mass; lower damping creates more oscillations.
- Keep limits anatomical. Increasing limits to make motion obvious usually
  creates mesh collapse; use better skin weights and corrective shapes instead.
- Use zero mobility to lock axes constrained by the surrounding body.
- Gravity should be zero while the character and tissue are in free fall. This
  avoids treating free fall as a standing load.
- Controller state is small and the solver is CPU-only. Work scales linearly
  with configured regions and active morph bindings; missing helper bones have
  no per-frame simulation cost beyond the region skip.

Cloth, hair, loose accessories, and large hanging flesh that needs environment
collision should use the existing Jolt soft-body paths. Body dynamics is for
bounded deformation attached to an animated skeleton; the two systems can run
together on the same character.

## Runnable example

The repository includes a procedural two-region rig that accelerates, lands,
rebounds, and turns. It prints helper-bone displacement and the generated
compression/impact morph weights, and requires neither Jolt nor a GPU:

```sh
cmake --preset linux -DRX_BUILD_EXAMPLES=ON
cmake --build build/linux --target body_jiggle_example
build/linux/examples/body_jiggle_example
```

See [`examples/body_jiggle.cc`](../examples/body_jiggle.cc) for the complete
per-frame integration.
