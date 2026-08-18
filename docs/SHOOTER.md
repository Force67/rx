# FPS range (`--demo shooter`)

The reference environment for `rx::combat`: a first-person shooting gallery
where a player walks the range on the character controller, carries four weapons
cut from the same `WeaponDef` struct, and shoots targets with head and torso
hitboxes. It is the tuning environment for weapon feel and the end-to-end
exercise of every combat system.

Everything lives in `runtime/demo_shooter.{h,cc}`. It touches no engine
internals — it drives `rx::combat`, `rx::character` and the scene camera rig
through their public APIs exactly as a game would. The registry wiring is the
usual few lines: a `shooter_` member on `DemoScenes`, a dispatch arm in
`CreateDemoScene`, a `shooter_->Emit(...)` call in `EmitToView`, and (because it
owns its camera + input, like the gym) a `shooter()->Update(...)` route in
`Viewer::OnUpdate`.

Run it on a real GPU:

```
vkrun ./build/linux/runtime/rx --demo shooter
```

## Controls

| Input | Action |
| --- | --- |
| **WASD** / left stick | Move (camera-relative) |
| **Mouse** / right stick | Look |
| **LMB** | Fire (held; semi-auto latches, the automatic paces itself) |
| **RMB** | Aim down sights (hold): tighter cone, narrower fov, softer recoil, slower look |
| **R** | Reload (a dry trigger pull also starts one) |
| **1 – 4** / scroll | Raise a weapon (the swap takes that weapon's `swap_time`) |
| **Shift** | Sprint (hold) — and watch the cone open |
| **Ctrl** | Crouch (hold) — and watch it close |
| **Space** | Jump |
| **G** | Reset the player, health and ammo |
| **M** | Hide / show the tuning panel |
| **Tab** | Release / recapture the cursor so the panel is clickable |
| **F1** | Toggle the renderer debug overlay |

## The range

- **Lanes** marked every 5 m out to 25 m, so damage falloff can be watched
  changing as you back off.
- **Seven targets**: five static, two strafing (lead them, especially with the
  launcher). Each is an entity with `Health` + `Damageable` + `Team`, plus a
  kinematic torso box and a smaller head box registered with the
  `combat::HitRegistry`. A head shot doubles, per the default zone table. A
  downed target drops through the floor and pops back up three seconds later.
- **Cover**: a 0.1 m plank the rifle punches through (at 60% damage) and a 1.0 m
  block it cannot — the same thickness-budget rule the module documents.
- **Crates**: dynamic bodies registered with an invalid entity, so they take
  bullet and blast impulses and never take damage.

## The arsenal

All four are the same `WeaponDef` with different numbers. The engine has no idea
what a "shotgun" is.

| Weapon | What it demonstrates |
| --- | --- |
| **rifle** | full auto at 660 rpm, bloom that opens under sustained fire, one pass-through of thin cover, 30-round magazine |
| **shotgun** | 9 pellets in a wide cone, brutal falloff past 6 m, **shell-by-shell reload** that firing interrupts |
| **marksman** | semi-auto, 55 damage, real aim-down-sights (0.45 fov scale), two pass-throughs, tight cone only while aimed |
| **launcher** | **projectile** rounds: 32 m/s muzzle speed, gravity and drag, a 5 m blast with falloff and impulse, and a fuse that detonates on expiry |

## The tuning panel (the point of the range)

Every `WeaponDef` field of the raised weapon is a live slider: rate of fire,
damage, falloff band and floor, spread min/max, bloom per shot and decay, aim
spread scale, recoil pitch/yaw/jitter, aim time and fov, reload time,
pass-throughs and thickness budget, blast radius and damage. Edits are
re-registered into the catalog under the same id, which is exactly what a data
reload does at runtime — so the feel is dialled in without a rebuild.

Below the sliders is the live state the systems are actually using: current
bloom, aim blend, cooldown, the cone half-angle the next round will leave in,
and the recoil's pending and recoverable amounts (with its recovery fraction and
half-life editable). The viewmodel's sway, bob and punch are there too.

## The HUD

- **Crosshair** sized from `combat::EffectiveSpread` — it *is* the cone the next
  round leaves in, so a running, jumping, spraying player watches it open.
- **Hit marker** on any damage the player dealt, **damage numbers** floating in
  the world (head shots in gold), and a `DOWN` popup on a kill.
- Ammo (magazine / reserve), reload and swap state, health bar, and a
  kills / hits / rounds tally.
- **Tracers** from the muzzle to each impact and **bullet holes** that lie flat
  on the surface they hit, both drained from `CombatEvents` — the engine emits
  events, the game decides what they look like.

## Capture hook

`RX_SHOOTER_AUTOFIRE=1` holds the trigger on the nearest live target every step
and releases the cursor, so a screenshot or headless run exercises firing,
impacts, kills and reloads without a hand on the mouse (the same idea as the
gym's `RX_GYM_SCRIPT`). Pair it with the viewer's capture hook:

```
RX_SHOOTER_AUTOFIRE=1 RX_UI_SHOT=/tmp/shot.png RX_UI_SHOT_FRAMES=140 \
  vkrun ./build/linux/runtime/rx --demo shooter
```

`RX_FIXED_DT` overrides the simulation step, as in the gym.

## What it does not do

Nothing shoots back — there is no AI in the range, so the player's `Health` and
the regeneration on it only ever come into play when standing in a blast. The
targets are boxes, not skeletons: hitbox bodies are placed by hand rather than
driven from an animated rig. Both are a game's job; the module's contract is the
`HitRegistry`, and the range shows the smallest thing that satisfies it.
