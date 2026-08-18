#ifndef RX_COMBAT_WEAPON_H_
#define RX_COMBAT_WEAPON_H_

#include "combat/components.h"
#include "combat/damage.h"
#include "combat/events.h"
#include "combat/weapon_def.h"
#include "core/export.h"
#include "physics/physics_world.h"

namespace rx::ecs {
class World;
}

// The firing half of the shooter: trigger discipline, rate of fire, spread and
// bloom, recoil, reloading, weapon switching, aim-down-sights, and the hitscan
// resolve (falloff, penetration, impulse). Projectile weapons stage their round
// here and hand it to StepProjectiles.
//
// Every system is a free function over plain-data components, staged by the
// game like the character controller's. Nothing here decides what a weapon IS:
// definitions are game data (see weapon_def.h) and presentation is a game
// draining CombatEvents.
namespace rx::combat {

// Stage A. Consumes each entity's WeaponIntent: advances swap/reload/cooldown
// clocks, fires whatever the trigger and fire mode allow, resolves hitscan
// rounds against `physics` (mapping bodies to targets through `registry`),
// spawns projectiles for kProjectile weapons, and pushes shot/impact/damage
// events. Structural: it creates projectile entities after iteration, so call
// it outside another World::Each.
RX_COMBAT_EXPORT void StepWeapons(ecs::World& world, physics::PhysicsWorld& physics,
                                  const WeaponCatalog& catalog, const HitRegistry& registry,
                                  CombatEvents& events, f32 dt);

// Stage B. Drains each ViewRecoil's pending kick and pulls the recoverable part
// back out, leaving this step's rotation in ViewRecoil::view_pitch_delta /
// view_yaw_delta. Run it BEFORE the game fills its look input for the step, and
// add those two into the look deltas it writes: recoil then goes through the
// same path as the mouse, so the player can fight it, and the module needs no
// opinion about whether the view yaw lives on a character or a camera.
RX_COMBAT_EXPORT void StepViewRecoil(ecs::World& world, f32 dt);

// Stage C. Updates the first-person weapon pose (sway from look input, bob from
// movement, aim-down-sights pull-in, per-shot punch). Presentation only: no
// other system reads Viewmodel.
RX_COMBAT_EXPORT void StepViewmodels(ecs::World& world, f32 dt);

// --- loadout helpers ------------------------------------------------------

// The raised weapon, or nullptr for an empty loadout.
RX_COMBAT_EXPORT WeaponState* ActiveWeapon(Loadout& loadout);
RX_COMBAT_EXPORT const WeaponState* ActiveWeapon(const Loadout& loadout);

// Puts `def` in the next free slot with a full magazine and `reserve` carried
// rounds. Returns the slot, or -1 when the loadout is full or the def unknown.
RX_COMBAT_EXPORT i8 GiveWeapon(Loadout& loadout, const WeaponCatalog& catalog, WeaponDefId def,
                               u32 reserve);

// Adds up to `rounds` to the reserve of every slot holding `def` (bounded by
// reserve_max). Returns what was taken; 0 means the pickup should stay on the
// floor.
RX_COMBAT_EXPORT u32 AddAmmo(Loadout& loadout, const WeaponCatalog& catalog, WeaponDefId def,
                             u32 rounds);

// Begins a reload if one makes sense (magazine not full, rounds in reserve, not
// already reloading). Returns whether it started.
RX_COMBAT_EXPORT bool StartReload(WeaponState& weapon, const WeaponDef& def);

// The cone half-angle (radians) the next round would leave in, given the
// weapon's bloom and the shooter's stance. A HUD sizes its crosshair with this.
RX_COMBAT_EXPORT f32 EffectiveSpread(const WeaponDef& def, const WeaponState& weapon,
                                     const WeaponIntent& intent);

// Camera fov multiplier for the raised weapon's aim blend; 1 when nothing is
// aimed. The game folds it into its projection.
RX_COMBAT_EXPORT f32 AimFovScale(const Loadout& loadout, const WeaponCatalog& catalog);

}  // namespace rx::combat

#endif  // RX_COMBAT_WEAPON_H_
