#ifndef RX_COMBAT_EVENTS_H_
#define RX_COMBAT_EVENTS_H_

#include <base/containers/vector.h>

#include "combat/components.h"
#include "core/math.h"
#include "ecs/entity.h"

// What the combat systems produce besides mutated components. Presentation is
// not the engine's business: tracers, muzzle flashes, impact decals, hit
// markers, damage numbers and audio are all a game reading these queues after
// the step and drawing whatever it likes.
namespace rx::combat {

// One round (or one shotgun blast) left the barrel.
struct ShotEvent {
  ecs::Entity shooter{};
  WeaponDefId weapon = kInvalidWeaponDef;
  Vec3 origin = {0, 0, 0};
  Vec3 direction = {0, 0, -1};  // the aim direction, before per-pellet spread
  u32 pellets = 1;
  u32 ammo_left = 0;
};

// A hitscan round or projectile touched the world. `target` is the entity the
// hit body belongs to, or an invalid entity for scenery.
struct ImpactEvent {
  Vec3 position = {0, 0, 0};
  Vec3 normal = {0, 1, 0};
  Vec3 direction = {0, 0, -1};  // travel direction of the round
  u64 body = 0;                 // physics::BodyId that was hit
  ecs::Entity shooter{};
  ecs::Entity target{};
  HitZone zone = HitZone::kDefault;
  f32 damage = 0;  // damage this impact requested, before the target's scaling
  bool penetrated = false;  // the round carried on through this surface
};

// Health actually changed. `applied` is what came off hp after armor.
struct DamageEvent {
  ecs::Entity target{};
  ecs::Entity instigator{};
  f32 amount = 0;   // requested, after zone and falloff scaling
  f32 applied = 0;  // removed from hp (armor took the rest)
  f32 absorbed = 0;
  HitZone zone = HitZone::kDefault;
  Vec3 position = {0, 0, 0};
  Vec3 direction = {0, 0, -1};
  u64 source = 0;  // game tag, usually the WeaponDef's name_hash
  bool killed = false;
};

struct ExplosionEvent {
  Vec3 position = {0, 0, 0};
  f32 radius = 0;
  ecs::Entity instigator{};
  u64 source = 0;
};

// One frame's worth of combat output. A game clears it once per step, runs the
// systems, then drains it.
struct CombatEvents {
  base::Vector<ShotEvent> shots;
  base::Vector<ImpactEvent> impacts;
  base::Vector<DamageEvent> damage;
  base::Vector<ExplosionEvent> explosions;

  void Clear() {
    shots.clear();
    impacts.clear();
    damage.clear();
    explosions.clear();
  }
};

}  // namespace rx::combat

#endif  // RX_COMBAT_EVENTS_H_
