# rx::combat

The shooter half of a first-person game, in the same functional-first shape as
`rx::character`: **plain-data components**, **free-function systems**, no manager
classes, no singletons, and no game taxonomy. The module knows what a *round*
is, not what a *rocket launcher* is — an arsenal is `WeaponDef` data a game
registers, and presentation is a game draining `CombatEvents`.

It is optional, like `rx::inventory`: nothing else in the engine depends on it.
It depends on `rx::physics` (a shot is a query), `rx::scene` (a projectile
carries a `Transform`) and `rx::ecs`.

## What is in the box

| Area | What it covers |
| --- | --- |
| Trigger | semi / auto / burst, trigger latching, bursts that finish after the trigger is released, dry-fire auto-reload |
| Rate | `rpm` pacing, separate in-burst interval |
| Accuracy | per-shot bloom between `spread_min` and `spread_max`, decay, move / airborne / crouch / aim modifiers, uniform cone sampling, multi-pellet spreads |
| Recoil | per-shot pitch + yaw kick with jitter, a kick that drains over `kick_half_life`, and a recovery that pulls part of it back after `recovery_delay` |
| Ammunition | magazine + reserve, tactical vs empty reload times, per-round (shell) reloading that firing interrupts |
| Handling | aim-down-sights blend and fov scale, weapon-swap timing across a four-slot loadout |
| Hitscan | closest-hit resolve, linear damage falloff, per-body hit zones, physics impulse, penetration through thin surfaces with a damage tax |
| Projectiles | ballistic rounds with gravity and per-metre drag, ray or swept-sphere collision, direct damage plus optional blast, fused detonation on expiry |
| Explosions | radial falloff, line-of-sight cover checks, impulses into every registered body, opt-in self-damage (rocket jumps) |
| Health | hp, armor with a per-hit absorb fraction, delayed regeneration, teams with friendly fire, per-zone and per-body multipliers |
| Presentation | shot / impact / damage / explosion event queues, and a viewmodel pose (sway, bob, aim pull-in, per-shot punch) |

## Components (data)

| Component | Role |
| --- | --- |
| `Loadout` | up to four `WeaponState` slots, the active one, and the swap timer |
| `WeaponState` | ammo, reserve, cooldown, reload and burst state, bloom, aim blend, the deterministic jitter stream |
| `WeaponIntent` | filled every fixed step: trigger / aim / reload / switch, the muzzle origin and aim direction, and the shooter's speed / airborne / crouched state that spread reads |
| `ViewRecoil` | pending kick, recoverable amount, and the `view_pitch_delta` / `view_yaw_delta` this step |
| `Viewmodel` | first-person weapon pose, view space (+x right, +y up, +z forward) |
| `Health` / `Damageable` / `Team` | hp + armor + regen; zone multiplier table, global damage scale, hit centre; team id + friendly fire |
| `HitIgnoreList` | bodies an entity's own casts skip (its hitboxes, its vehicle) |
| `Projectile` | a round in flight: position, velocity, drag, fuse, blast |

`WeaponDef` (weapon_def.h) is the immutable tuning half, held in a game-owned
`WeaponCatalog` passed explicitly to the systems — there is no global catalog,
for the same reason `inventory::ItemCatalog` has none.

## The HitRegistry

A shot resolves what it hit through one table: `physics::BodyId -> {entity,
zone, multiplier}`. That indirection is the whole coupling contract, and it is
explicit because **a character controller has no physics body at all** (Jolt's
`CharacterVirtual` is not in the broadphase). A game therefore builds hitboxes
as kinematic bodies it moves with its skeleton and registers them:

```cpp
registry.Register(torso_body, enemy, combat::HitZone::kTorso);
registry.Register(head_body,  enemy, combat::HitZone::kHead);
```

One capsule per enemy for a cheap game, a head/torso/limb set for a precise one.
Register a barrel's body with an invalid entity and it takes impulses and blast
pushes without ever taking damage. `UnregisterTarget(entity)` drops the lot.

## Systems (free functions)

- `StepWeapons(world, physics, catalog, registry, events, dt)` — the whole
  firing pipeline: swap and reload clocks, fire-mode gating, spread, recoil,
  hitscan resolve and projectile staging. Structural (it creates projectile
  entities after its walk), so call it outside another `World::Each`.
- `StepProjectiles(world, physics, registry, events, dt)` — integrates rounds in
  flight and sweeps the segment each one covered this step.
- `StepViewRecoil(world, dt)` — turns kick into `view_*_delta`.
- `StepViewmodels(world, dt)` — sway, bob, aim pull-in, punch.
- `StepHealth(world, dt)` — the regeneration clock.
- `ApplyDamage` / `Heal` / `CanDamage` / `ApplyExplosion` — the damage verbs, all
  usable on their own (a trap, a fall, a scripted execution).

Helpers: `GiveWeapon`, `AddAmmo`, `StartReload`, `ActiveWeapon`,
`EffectiveSpread` (a HUD sizes its crosshair with it), `AimFovScale`.

## Recoil is a delta, not an angle

`StepViewRecoil` does **not** write the camera. It leaves the rotation recoil
wants for this step in `ViewRecoil::view_pitch_delta` / `view_yaw_delta`, and the
game adds those into whatever look input it already fills:

```cpp
combat::StepViewRecoil(world, dt);
const auto& recoil = *world.Get<combat::ViewRecoil>(player);
character_intent.look_yaw_delta   += recoil.view_yaw_delta;
character_intent.look_pitch_delta += recoil.view_pitch_delta;
```

Recoil then travels the same path as the mouse, so the player fights it with the
mouse, and the module needs no opinion about whether the view yaw lives on a
character heading or a free camera. (In first person the character owns the yaw
and the camera owns the pitch, so a module that wrote `scene::CameraIntent`
directly would silently drop every horizontal kick.)

## Per-fixed-step call order

```
1. combat::StepViewRecoil(world, dt)         // before look input is filled
2. fill character look/move intent           // + the two recoil deltas
3. character::StepCharacters(...)            // and the camera rig stages
   ... scene::ResolveCameraStacks(world, dt)
4. fill combat::WeaponIntent                 // origin/direction from the resolved camera
5. combat::StepWeapons(world, physics, catalog, registry, events, dt)
6. combat::StepProjectiles(world, physics, registry, events, dt)
7. combat::StepHealth(world, dt)
8. combat::StepViewmodels(world, dt)
9. physics.Update(dt), then drain `events` for tracers, decals, audio, HUD
```

The weapon intent is filled **after** the camera resolves, because the muzzle
points where the player is actually aiming, not where they were aiming last
step.

## Determinism

Spread and recoil jitter come from a per-weapon `u32` stream (the same PCG hash
`rx::placement` uses), seeded per slot by `GiveWeapon`. The same inputs and the
same fixed steps produce the same pellets, which is what a replay or a
server-side re-simulation needs. No calls to `rand()`, no clock reads.

## Honest limits

- **Penetration is a thickness budget, not a material.** After a surface, the
  ray resumes `penetration` metres further along; if it lands back on the same
  body the round stops there, so anything thicker than the budget is cover.
  There is no per-material table, and no exit-point solve.
- **Swept projectiles cannot pre-filter the owner.** `PhysicsWorld::SphereCast`
  has no ignore-list overload, so a projectile with a radius discards hits on
  its owner's bodies after the fact instead of before.
- **Explosion impulses reach registered bodies only.** `ApplyExplosion` pushes
  what is in the `HitRegistry`; scenery a game never registered is not moved.
- **No damage-over-time, no status effects, no melee.** They are a game's job
  on top of `ApplyDamage`.

## Physics API addition

`PhysicsWorld::RayHit` gained a `body` field (real backend and stub). Without it
a shot knows where it landed but not *what* it hit, which is the one thing a
shooter needs. `SphereCast` fills it too.

## Tested

`test/combat_test.cc` runs headless against real Jolt: rate of fire and fire
modes, magazine/reserve and per-round reloading, weapon swap timing, spread
bloom and aim, view-recoil kick and recovery totals, falloff, head-shot zones,
armor and regeneration, friendly fire, penetration through thin cover versus
thick, projectile drop, explosion falloff and line-of-sight, and registry
lifetime.
