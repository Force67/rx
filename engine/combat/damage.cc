#include "combat/damage.h"

#include <algorithm>
#include <cmath>

#include "ecs/world.h"
#include "scene/components.h"

namespace rx::combat {
namespace {

f32 ZoneMultiplier(const Damageable* tuning, HitZone zone) {
  if (!tuning) return 1.0f;
  const u32 index = static_cast<u32>(zone);
  return index < kHitZoneCount ? tuning->zone_multiplier[index] : 1.0f;
}

Vec3 DamageCenter(const scene::Transform& transform, const Damageable* tuning) {
  Vec3 p{transform.position[0], transform.position[1], transform.position[2]};
  if (tuning) p += tuning->center_offset;
  return p;
}

}  // namespace

void HitRegistry::Register(physics::BodyId body, ecs::Entity target, HitZone zone,
                           f32 multiplier) {
  if (body == 0) return;
  HitProxy& proxy = bodies_[body];
  proxy.target = target;
  proxy.zone = zone;
  proxy.multiplier = multiplier;
}

void HitRegistry::Unregister(physics::BodyId body) { bodies_.erase(body); }

u32 HitRegistry::UnregisterTarget(ecs::Entity target) {
  // Collect first: erasing during ForEach would move slots under the walk.
  base::Vector<physics::BodyId> doomed;
  bodies_.ForEach([&](physics::BodyId body, const HitProxy& proxy) {
    if (proxy.target == target) doomed.push_back(body);
  });
  for (physics::BodyId body : doomed) bodies_.erase(body);
  return static_cast<u32>(doomed.size());
}

const HitProxy* HitRegistry::Find(physics::BodyId body) const {
  if (body == 0) return nullptr;
  return bodies_.find(body);
}

void HitRegistry::Clear() { bodies_.clear(); }

bool CanDamage(ecs::World& world, ecs::Entity instigator, ecs::Entity target) {
  if (!target) return false;
  if (instigator == target) return false;
  if (!instigator) return true;  // world damage (a trap, the void) hits everyone
  const Team* mine = world.Get<Team>(instigator);
  const Team* theirs = world.Get<Team>(target);
  if (!mine || !theirs) return true;
  if (mine->id != theirs->id) return true;
  return mine->friendly_fire;
}

bool ApplyDamage(ecs::World& world, const DamageRequest& request, CombatEvents* events) {
  Health* health = world.Get<Health>(request.target);
  if (!health || health->dead) return false;
  const Damageable* tuning = world.Get<Damageable>(request.target);
  if (tuning && tuning->invulnerable) return false;

  f32 amount = request.amount * request.multiplier * ZoneMultiplier(tuning, request.zone);
  if (tuning) amount *= tuning->damage_scale;
  if (!(amount > 0)) return false;

  health->time_since_damage = 0;

  f32 absorbed = 0;
  if (health->armor > 0) {
    absorbed = std::min(health->armor, amount * std::clamp(health->armor_absorb, 0.0f, 1.0f));
    health->armor -= absorbed;
  }
  const f32 applied = amount - absorbed;
  if (applied <= 0) {
    if (events) {
      DamageEvent event;
      event.target = request.target;
      event.instigator = request.instigator;
      event.amount = amount;
      event.absorbed = absorbed;
      event.zone = request.zone;
      event.position = request.position;
      event.direction = request.direction;
      event.source = request.source;
      events->damage.push_back(event);
    }
    return false;
  }

  health->hp -= applied;
  const bool killed = health->hp <= 0;
  if (killed) {
    health->hp = 0;
    health->dead = true;
  }

  if (events) {
    DamageEvent event;
    event.target = request.target;
    event.instigator = request.instigator;
    event.amount = amount;
    event.applied = applied;
    event.absorbed = absorbed;
    event.zone = request.zone;
    event.position = request.position;
    event.direction = request.direction;
    event.source = request.source;
    event.killed = killed;
    events->damage.push_back(event);
  }
  return true;
}

f32 Heal(ecs::World& world, ecs::Entity entity, f32 amount) {
  Health* health = world.Get<Health>(entity);
  if (!health || health->dead || amount <= 0) return 0;
  const f32 given = std::min(amount, health->max_hp - health->hp);
  if (given <= 0) return 0;
  health->hp += given;
  return given;
}

void StepHealth(ecs::World& world, f32 dt) {
  if (!std::isfinite(dt) || dt <= 0) return;
  world.Each<Health>([dt](ecs::Entity, Health& health) {
    if (health.dead) return;
    health.time_since_damage += dt;
    if (health.regen_rate <= 0) return;
    if (health.time_since_damage < health.regen_delay) return;
    health.hp = std::min(health.max_hp, health.hp + health.regen_rate * dt);
  });
}

u32 ApplyExplosion(ecs::World& world, physics::PhysicsWorld& physics, const HitRegistry& registry,
                   const ExplosionParams& params, CombatEvents* events) {
  if (params.radius <= 0) return 0;

  if (events) {
    ExplosionEvent event;
    event.position = params.position;
    event.radius = params.radius;
    event.instigator = params.instigator;
    event.source = params.source;
    events->explosions.push_back(event);
  }

  const f32 min_scale = std::clamp(params.min_scale, 0.0f, 1.0f);
  u32 damaged = 0;

  world.Each<Health, scene::Transform>([&](ecs::Entity entity, Health& health,
                                           scene::Transform& transform) {
    if (health.dead) return;
    const bool is_self = entity == params.instigator;
    if (is_self && !params.damage_self) return;
    if (!is_self && !CanDamage(world, params.instigator, entity)) return;

    const Damageable* tuning = world.Get<Damageable>(entity);
    const Vec3 center = DamageCenter(transform, tuning);
    const Vec3 to_target = center - params.position;
    const f32 distance = Length(to_target);
    if (distance > params.radius) return;

    if (params.line_of_sight && distance > 1e-3f) {
      const Vec3 direction = to_target * (1.0f / distance);
      physics::PhysicsWorld::RayHit hit;
      if (physics.Raycast(params.position, direction, distance, &hit)) {
        // Anything in the way blocks the blast, unless the thing in the way is
        // the target's own hitbox.
        const HitProxy* proxy = registry.Find(hit.body);
        if (!proxy || proxy->target != entity) return;
      }
    }

    const f32 t = distance / params.radius;
    DamageRequest request;
    request.target = entity;
    request.instigator = params.instigator;
    request.amount = params.damage * (1.0f + (min_scale - 1.0f) * t);
    request.position = params.position;
    request.direction = distance > 1e-3f ? to_target * (1.0f / distance) : Vec3{0, 1, 0};
    request.source = params.source;
    if (ApplyDamage(world, request, events)) ++damaged;
  });

  if (params.impulse > 0) {
    registry.ForEach([&](physics::BodyId body, const HitProxy&) {
      Vec3 position;
      f32 rotation[4];
      if (!physics.GetBodyTransform(body, &position, rotation)) return;
      const Vec3 to_body = position - params.position;
      const f32 distance = Length(to_body);
      if (distance > params.radius) return;
      const f32 t = distance / params.radius;
      const f32 scale = params.impulse * (1.0f + (min_scale - 1.0f) * t);
      const Vec3 direction =
          distance > 1e-3f ? to_body * (1.0f / distance) : Vec3{0, 1, 0};
      physics.ApplyImpulse(body, direction * scale);
    });
  }

  return damaged;
}

}  // namespace rx::combat
