#ifndef RX_COMBAT_PROJECTILE_H_
#define RX_COMBAT_PROJECTILE_H_

#include "combat/components.h"
#include "combat/damage.h"
#include "combat/events.h"
#include "core/export.h"
#include "physics/physics_world.h"

namespace rx::ecs {
class World;
}

// Rounds that travel: grenades, rockets, arrows, plasma. They are ECS entities
// with a Projectile and a scene::Transform, integrated with gravity and drag and
// swept against the physics world every step, so they can be led, dropped, and
// dodged. Fast rounds stay hitscan (see weapon.h); this is for the ones whose
// flight time is part of the gameplay.
namespace rx::combat {

// Creates a projectile entity from `desc` (its `position` seeds the Transform).
// Structural, so call it outside World::Each. StepWeapons uses it for
// kProjectile weapons; a game calls it directly for thrown things.
RX_COMBAT_EXPORT ecs::Entity SpawnProjectile(ecs::World& world, const Projectile& desc);

// Integrates every projectile, sweeps the segment it covered this step against
// the world, and resolves the first thing it touched: direct damage through
// `registry`, a blast if the definition has one, an impulse into the body it
// hit. Rounds that time out despawn (or detonate, for a fused grenade).
// Structural: expired and impacted projectiles are destroyed after iteration.
RX_COMBAT_EXPORT void StepProjectiles(ecs::World& world, physics::PhysicsWorld& physics,
                                      const HitRegistry& registry, CombatEvents& events, f32 dt);

}  // namespace rx::combat

#endif  // RX_COMBAT_PROJECTILE_H_
