# Procedural walk styles

`rx::anim::Locomotion` can apply reusable walk profiles directly to a bind
pose. Profiles describe motion rather than gender: choose them from character
metadata, customization, mood, equipment, or gameplay state.

The built-in presets are:

| Preset | Motion character |
| --- | --- |
| `kNeutral` | balanced stride, arm swing, and pelvis motion |
| `kHipSway` | stronger pelvis roll/shift, softer knees, restrained arms |
| `kMarch` | higher knees, longer stride, stronger arms, upright pelvis |

Every preset returns an ordinary `WalkStyle`, so games can tune individual
values or blend between profiles without adding another enum value:

```cpp
anim::Locomotion walk;
walk.style = anim::MakeWalkStylePreset(anim::WalkStyleKind::kHipSway);
walk.phase = anim::AdvancePhase(walk.phase, speed, dt, walk.style);
walk.Apply(skeleton, speed, &pose);
```

For a smooth state change, use `BlendWalkStyles` during the transition. Keep a
single phase clock while blending so feet do not jump to another point in the
stride.

The helper recognizes the engine's legacy `NPC ...` biped names and common
Genesis/Blender deform names (`hip`, `thigh.bend.L/R`, `shin.bend.L/R`,
`upper_arm.bend.L/R`, and `forearm.bend.L/R`). Unknown bones are skipped, so a
partial rig still receives the portions it supports.

Apply the walk before secondary body dynamics:

1. evaluate or reset the base pose;
2. apply `Locomotion`;
3. apply `BodyDynamics` and morph weights;
4. build model matrices and the skin palette.

In the editor, imported characters start in **Walk Auto**, which holds Hip Sway
and March for five seconds each with a one-second crossfade. The top bar also
offers **Hip Sway** and **March** buttons to lock either profile while tuning a
character.
