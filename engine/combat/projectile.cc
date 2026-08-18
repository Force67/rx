#include "combat/projectile.h"

#include <algorithm>
#include <cmath>

#include <base/containers/vector.h>

#include "ecs/world.h"
#include "scene/components.h"

namespace rx::combat {
namespace {

bool IsIgnored(const Projectile& round, physics::BodyId body) {
  for (u8 i = 0; i < round.ignore_count; ++i) {
    if (round.ignore[i] == body) return true;
  }
  return false;
}

void WriteTransform(ecs::World& world, ecs::Entity entity, const Vec3& position) {
  scene::Transform* transform = world.Get<scene::Transform>(entity);
  if (!transform) return;
  transform->position[0] = position.x;
  transform->position[1] = position.y;
  transform->position[2] = position.z;
}

ExplosionParams BlastFrom(const Projectile& round, const Vec3& position) {
  ExplosionParams blast;
  blast.position = position;
  blast.radius = round.blast_radius;
  blast.damage = round.blast_damage;
  blast.min_scale = round.blast_min_scale;
  blast.impulse = round.blast_impulse;
  blast.instigator = round.owner;
  blast.source = static_cast<u64>(round.def);
  return blast;
}

}  // namespace

ecs::Entity SpawnProjectile(ecs::World& world, const Projectile& desc) {
  const ecs::Entity entity = world.Create();
  world.Add(entity, desc);
  scene::Transform transform;
  transform.position[0] = desc.position.x;
  transform.position[1] = desc.position.y;
  transform.position[2] = desc.position.z;
  world.Add(entity, transform);
  return entity;
}

void StepProjectiles(ecs::World& world, physics::PhysicsWorld& physics,
                     const HitRegistry& registry, CombatEvents& events, f32 dt) {
  if (!std::isfinite(dt) || dt <= 0) return;

  base::Vector<ecs::Entity> spent;
  base::Vector<ExplosionParams> blasts;

  world.Each<Projectile>([&](ecs::Entity entity, Projectile& round) {
    round.age += dt;

    Vec3 velocity = round.velocity;
    velocity.y -= round.gravity * dt;
    if (round.drag > 0) {
      // drag is per metre travelled, so the decay over a step depends on how
      // far the round actually gets: fast rounds bleed speed faster.
      velocity = velocity * std::exp(-round.drag * Length(velocity) * dt);
    }

    const Vec3 from = round.position;
    const Vec3 step = velocity * dt;
    const f32 distance = Length(step);

    physics::PhysicsWorld::RayHit hit;
    bool touched = false;
    if (distance > 1e-6f) {
      const Vec3 direction = step * (1.0f / distance);
      if (round.radius > 0) {
        // SphereCast has no ignore-list overload, so a swept round filters the
        // owner's own bodies after the fact instead of before.
        touched = physics.SphereCast(from, direction, distance, round.radius, &hit) &&
                  !IsIgnored(round, hit.body);
      } else {
        touched = physics.Raycast(from, direction, distance, &hit, round.ignore,
                                  round.ignore_count);
      }

      if (touched) {
        round.position = hit.position;
        round.velocity = velocity;

        const HitProxy* proxy = registry.Find(hit.body);
        const ecs::Entity target = proxy ? proxy->target : ecs::kInvalidEntity;
        if (proxy && round.damage > 0 && CanDamage(world, round.owner, target)) {
          DamageRequest request;
          request.target = target;
          request.instigator = round.owner;
          request.amount = round.damage;
          request.zone = proxy->zone;
          request.multiplier = proxy->multiplier;
          request.position = hit.position;
          request.direction = direction;
          request.source = static_cast<u64>(round.def);
          ApplyDamage(world, request, &events);
        }
        if (round.impulse > 0 && hit.body != 0) {
          physics.ApplyImpulse(hit.body, direction * round.impulse);
        }

        ImpactEvent impact;
        impact.position = hit.position;
        impact.normal = hit.normal;
        impact.direction = direction;
        impact.body = hit.body;
        impact.shooter = round.owner;
        impact.target = target;
        impact.zone = proxy ? proxy->zone : HitZone::kDefault;
        impact.damage = round.damage;
        events.impacts.push_back(impact);

        if (round.blast_radius > 0) blasts.push_back(BlastFrom(round, hit.position));
        WriteTransform(world, entity, round.position);
        spent.push_back(entity);
        return;
      }
    }

    round.position = from + step;
    round.velocity = velocity;
    WriteTransform(world, entity, round.position);

    if (round.age >= round.life) {
      if (round.explode_on_expire && round.blast_radius > 0) {
        blasts.push_back(BlastFrom(round, round.position));
      }
      spent.push_back(entity);
    }
  });

  // Blasts run outside the walk: they iterate the world themselves, and a
  // chained detonation must not reshape rows under an active iteration.
  for (const ExplosionParams& blast : blasts) {
    ApplyExplosion(world, physics, registry, blast, &events);
  }
  for (ecs::Entity entity : spent) world.Destroy(entity);
}

}  // namespace rx::combat
