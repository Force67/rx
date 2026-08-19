#ifndef RX_COMBAT_DAMAGE_H_
#define RX_COMBAT_DAMAGE_H_

#include <base/containers/unordered_map.h>

#include "combat/components.h"
#include "combat/events.h"
#include "core/export.h"
#include "ecs/entity.h"
#include "physics/physics_world.h"

namespace rx::ecs {
class World;
}

namespace rx::combat {

// --- who owns a body ------------------------------------------------------

// What a physics body means to the combat systems.
struct HitProxy {
  ecs::Entity target{};
  HitZone zone = HitZone::kDefault;
  f32 multiplier = 1.0f;  // extra per-body scaling on top of the zone table
};

// Maps physics bodies to the entity they belong to. A shot resolves what it hit
// through this and nothing else, which is what keeps the module free of any
// opinion about how a game builds its characters: register one capsule per
// enemy for a cheap game, a head/torso/limb set for a precise one, a barrel's
// body for explodable scenery.
//
// It has to be explicit because a character controller has no body at all
// (Jolt's CharacterVirtual is not in the broadphase), so hitboxes are separate
// kinematic bodies the game moves with its skeleton.
class RX_COMBAT_EXPORT HitRegistry {
 public:
  // Registers (or re-points) `body`. `multiplier` stacks with the target's
  // Damageable zone table, for a body that is not just its zone (a helmet).
  void Register(physics::BodyId body, ecs::Entity target, HitZone zone = HitZone::kDefault,
                f32 multiplier = 1.0f);
  void Unregister(physics::BodyId body);
  // Drops every body registered to `target` (call when it dies or despawns).
  // Returns the number removed.
  u32 UnregisterTarget(ecs::Entity target);
  // Proxy for `body`, or nullptr when the body is plain scenery.
  const HitProxy* Find(physics::BodyId body) const;
  void Clear();
  mem_size size() const { return bodies_.size(); }

  // fn(physics::BodyId, const HitProxy&) over every registration.
  template <typename Fn>
  void ForEach(Fn&& fn) const {
    bodies_.ForEach(fn);
  }

 private:
  base::UnorderedMap<physics::BodyId, HitProxy> bodies_;
};

// --- damage ---------------------------------------------------------------

struct DamageRequest {
  ecs::Entity target{};
  ecs::Entity instigator{};
  f32 amount = 0;  // before the target's zone table, damage_scale and armor
  HitZone zone = HitZone::kDefault;
  f32 multiplier = 1.0f;  // per-body extra scaling (HitProxy::multiplier)
  Vec3 position = {0, 0, 0};
  Vec3 direction = {0, 0, -1};
  u64 source = 0;  // game tag, usually the WeaponDef's name_hash
};

// True when `instigator` is allowed to hurt `target`: never itself, and never a
// team-mate unless the INSTIGATOR's Team has friendly fire on (the shooter's
// rules of engagement decide, not the victim's). Entities without a Team are
// hostile to everyone.
RX_COMBAT_EXPORT bool CanDamage(ecs::World& world, ecs::Entity instigator, ecs::Entity target);

// Applies `request` to the target's Health, scaled by its Damageable zone table
// and damage_scale, with armor soaking `armor_absorb` of what is left. Pushes a
// DamageEvent when anything landed. Returns true when hp actually moved: false
// for a missing/invulnerable/already-dead target or a fully absorbed hit.
RX_COMBAT_EXPORT bool ApplyDamage(ecs::World& world, const DamageRequest& request,
                                  CombatEvents* events = nullptr);

// Restores up to `amount` hp (never past max_hp, never on the dead). Returns
// what it gave back.
RX_COMBAT_EXPORT f32 Heal(ecs::World& world, ecs::Entity entity, f32 amount);

// Advances the regeneration clock on every Health and hands back hp once
// `regen_delay` has passed without damage.
RX_COMBAT_EXPORT void StepHealth(ecs::World& world, f32 dt);

// --- explosions -----------------------------------------------------------

struct ExplosionParams {
  Vec3 position = {0, 0, 0};
  f32 radius = 5.0f;
  f32 damage = 100.0f;   // at the centre
  f32 min_scale = 0.15f; // damage scale at the rim
  f32 impulse = 0;       // N*s at the centre, pushed into dynamic bodies
  ecs::Entity instigator{};
  u64 source = 0;
  // Cast a ray to each candidate and skip the ones behind cover. Off makes the
  // blast pass through walls (and costs no queries).
  bool line_of_sight = true;
  // Whether the instigator (and its team, per CanDamage) can be caught in it.
  // Rocket jumping wants this on.
  bool damage_self = true;
};

// Damages every Health inside the radius with linear falloff, and pushes every
// registered body inside it. Returns the number of entities damaged.
RX_COMBAT_EXPORT u32 ApplyExplosion(ecs::World& world, physics::PhysicsWorld& physics,
                                    const HitRegistry& registry, const ExplosionParams& params,
                                    CombatEvents* events = nullptr);

}  // namespace rx::combat

#endif  // RX_COMBAT_DAMAGE_H_
