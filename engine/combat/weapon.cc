#include "combat/weapon.h"

#include <algorithm>
#include <cmath>

#include <base/containers/vector.h>

#include "combat/projectile.h"
#include "ecs/world.h"

namespace rx::combat {
namespace {

constexpr f32 kTwoPi = 6.28318530717958647692f;

// The shot cooldown is counted down in fixed steps, so it lands on a value that
// is zero only to within float error: 600 rpm at 60 Hz leaves 6e-9 s on the
// clock after six steps, and a strict `> 0` test would hold the round for a
// seventh step and quietly cost the weapon 14% of its rate of fire. Anything
// under this many seconds counts as ready.
constexpr f32 kCooldownEpsilon = 1e-5f;

// The hash the placement module uses, run as a stream: deterministic per weapon,
// so a replay or a server re-simulating the same inputs gets the same pellets.
u32 NextRandom(u32& state) {
  state = state * 747796405u + 2891336453u;
  const u32 word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}

f32 RandomUnit(u32& state) {
  return static_cast<f32>(NextRandom(state) >> 8u) * (1.0f / 16777216.0f);
}

f32 RandomSigned(u32& state) { return RandomUnit(state) * 2.0f - 1.0f; }

f32 MoveTowardScalar(f32 current, f32 target, f32 max_delta) {
  const f32 d = target - current;
  if (std::abs(d) <= max_delta) return target;
  return current + (d > 0 ? max_delta : -max_delta);
}

// Frame-rate-independent exponential approach, as in character.cc: the fraction
// of the remaining gap closed over `dt` for a given half-life.
f32 ApproachFraction(f32 half_life, f32 dt) {
  if (half_life <= 0) return 1.0f;
  return 1.0f - std::exp2(-dt / half_life);
}

void Basis(const Vec3& forward, Vec3* right, Vec3* up) {
  const Vec3 reference = std::abs(forward.y) > 0.99f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
  *right = Normalize(Cross(forward, reference));
  *up = Cross(*right, forward);
}

// Uniform direction inside a cone of half-angle `spread` around `forward`. The
// sqrt keeps samples even over the cap instead of clustering on the axis, so a
// shotgun pattern looks like a pattern and not like a dot with outliers.
Vec3 ConeDirection(const Vec3& forward, f32 spread, u32& rng) {
  if (spread <= 0) return forward;
  const f32 angle = RandomUnit(rng) * kTwoPi;
  const f32 radius = std::sqrt(RandomUnit(rng)) * std::tan(spread);
  Vec3 right;
  Vec3 up;
  Basis(forward, &right, &up);
  return Normalize(forward + right * (std::cos(angle) * radius) +
                   up * (std::sin(angle) * radius));
}

Vec3 SafeDirection(const Vec3& v) {
  const f32 length = Length(v);
  if (!std::isfinite(length) || length < 1e-6f) return {0, 0, -1};
  return v * (1.0f / length);
}

void CompleteMagazineReload(WeaponState& weapon, const WeaponDef& def) {
  const u32 needed = def.magazine > weapon.ammo ? def.magazine - weapon.ammo : 0;
  const u32 taken = std::min(needed, weapon.reserve);
  weapon.ammo += taken;
  weapon.reserve -= taken;
  weapon.reload_timer = 0;
  weapon.reload_shells = false;
  weapon.reload_empty = false;
}

// Everything a single round needs to resolve itself against the world.
struct ShotContext {
  ecs::World* world = nullptr;
  physics::PhysicsWorld* physics = nullptr;
  const HitRegistry* registry = nullptr;
  CombatEvents* events = nullptr;
  base::Vector<ExplosionParams>* blasts = nullptr;
  ecs::Entity shooter{};
  const physics::BodyId* ignore = nullptr;
  u32 ignore_count = 0;
};

void ResolveHitscan(const ShotContext& ctx, const WeaponDef& def, const Vec3& origin,
                    const Vec3& direction) {
  f32 travelled = 0;
  f32 damage_scale = 1.0f;
  u32 pierced = 0;
  Vec3 from = origin;
  physics::BodyId previous_body = 0;

  while (travelled < def.range) {
    physics::PhysicsWorld::RayHit hit;
    if (!ctx.physics->Raycast(from, direction, def.range - travelled, &hit, ctx.ignore,
                              ctx.ignore_count)) {
      return;
    }
    // The resumed ray landed back on the surface it was meant to pass through:
    // that surface is thicker than the penetration budget, so the round stops
    // inside it without a second impact.
    if (hit.body != 0 && hit.body == previous_body) return;

    travelled += hit.distance;
    const f32 damage = def.damage * FalloffScale(def, travelled) * damage_scale;

    const HitProxy* proxy = ctx.registry->Find(hit.body);
    const ecs::Entity target = proxy ? proxy->target : ecs::kInvalidEntity;
    if (proxy && CanDamage(*ctx.world, ctx.shooter, target)) {
      DamageRequest request;
      request.target = target;
      request.instigator = ctx.shooter;
      request.amount = damage;
      request.zone = proxy->zone;
      request.multiplier = proxy->multiplier;
      request.position = hit.position;
      request.direction = direction;
      request.source = def.name_hash;
      ApplyDamage(*ctx.world, request, ctx.events);
    }
    if (def.impulse > 0 && hit.body != 0) {
      ctx.physics->ApplyImpulse(hit.body, direction * def.impulse);
    }

    const bool penetrates = pierced < def.max_penetrations && def.penetration > 0;

    ImpactEvent impact;
    impact.position = hit.position;
    impact.normal = hit.normal;
    impact.direction = direction;
    impact.body = hit.body;
    impact.shooter = ctx.shooter;
    impact.target = target;
    impact.zone = proxy ? proxy->zone : HitZone::kDefault;
    impact.damage = damage;
    impact.penetrated = penetrates;
    ctx.events->impacts.push_back(impact);

    if (def.blast_radius > 0) {
      ExplosionParams blast;
      blast.position = hit.position;
      blast.radius = def.blast_radius;
      blast.damage = def.blast_damage;
      blast.min_scale = def.blast_min_scale;
      blast.impulse = def.blast_impulse;
      blast.instigator = ctx.shooter;
      blast.source = def.name_hash;
      ctx.blasts->push_back(blast);
    }

    if (!penetrates) return;
    ++pierced;
    damage_scale *= def.penetration_damage_scale;
    previous_body = hit.body;
    from = hit.position + direction * def.penetration;
    travelled += def.penetration;
  }
}

}  // namespace

WeaponState* ActiveWeapon(Loadout& loadout) {
  if (loadout.count == 0) return nullptr;
  if (loadout.active >= loadout.count) return nullptr;
  return &loadout.slots[loadout.active];
}

const WeaponState* ActiveWeapon(const Loadout& loadout) {
  if (loadout.count == 0) return nullptr;
  if (loadout.active >= loadout.count) return nullptr;
  return &loadout.slots[loadout.active];
}

i8 GiveWeapon(Loadout& loadout, const WeaponCatalog& catalog, WeaponDefId def_id, u32 reserve) {
  const WeaponDef* def = catalog.Find(def_id);
  if (!def || loadout.count >= kMaxWeaponSlots) return -1;
  const u8 slot = loadout.count++;
  WeaponState& weapon = loadout.slots[slot];
  weapon = WeaponState{};
  weapon.def = def_id;
  weapon.ammo = def->magazine;
  weapon.reserve = def->reserve_max > 0 ? std::min(reserve, def->reserve_max) : reserve;
  // A distinct jitter stream per slot, so two identical rifles do not fire the
  // same pattern in lockstep.
  weapon.rng = 0x9E3779B9u ^ (def_id * 0x85EBCA6Bu) ^ ((slot + 1u) * 0xC2B2AE35u);
  return static_cast<i8>(slot);
}

u32 AddAmmo(Loadout& loadout, const WeaponCatalog& catalog, WeaponDefId def_id, u32 rounds) {
  const WeaponDef* def = catalog.Find(def_id);
  if (!def || rounds == 0) return 0;
  u32 taken = 0;
  for (u8 i = 0; i < loadout.count && taken < rounds; ++i) {
    WeaponState& weapon = loadout.slots[i];
    if (weapon.def != def_id) continue;
    const u32 room = def->reserve_max > 0
                         ? (def->reserve_max > weapon.reserve ? def->reserve_max - weapon.reserve
                                                              : 0)
                         : rounds - taken;
    const u32 give = std::min(room, rounds - taken);
    weapon.reserve += give;
    taken += give;
  }
  return taken;
}

bool StartReload(WeaponState& weapon, const WeaponDef& def) {
  if (def.magazine == 0) return false;          // bottomless: nothing to reload
  if (weapon.reload_timer > 0) return false;    // already going
  if (weapon.ammo >= def.magazine) return false;
  if (weapon.reserve == 0) return false;

  weapon.burst_remaining = 0;
  if (def.reload_shell_time > 0) {
    weapon.reload_shells = true;
    weapon.reload_empty = weapon.ammo == 0;
    weapon.reload_timer = def.reload_shell_time;
    return true;
  }

  weapon.reload_shells = false;
  weapon.reload_empty = weapon.ammo == 0;
  const f32 duration =
      weapon.reload_empty && def.reload_empty_time > 0 ? def.reload_empty_time : def.reload_time;
  if (duration <= 0) {
    CompleteMagazineReload(weapon, def);
    return true;
  }
  weapon.reload_timer = duration;
  return true;
}

f32 EffectiveSpread(const WeaponDef& def, const WeaponState& weapon, const WeaponIntent& intent) {
  const f32 bloom = std::clamp(weapon.bloom, 0.0f, 1.0f);
  f32 spread = def.spread_min + (def.spread_max - def.spread_min) * bloom;
  if (intent.airborne) {
    spread *= def.spread_air_scale;
  } else {
    const f32 move = def.spread_move_speed > 0
                         ? std::clamp(intent.speed / def.spread_move_speed, 0.0f, 1.0f)
                         : 0.0f;
    spread *= 1.0f + (def.spread_move_scale - 1.0f) * move;
    if (intent.crouched) spread *= def.spread_crouch_scale;
  }
  spread *= 1.0f + (def.spread_ads_scale - 1.0f) * std::clamp(weapon.ads, 0.0f, 1.0f);
  return std::max(spread, 0.0f);
}

f32 AimFovScale(const Loadout& loadout, const WeaponCatalog& catalog) {
  const WeaponState* weapon = ActiveWeapon(loadout);
  if (!weapon) return 1.0f;
  const WeaponDef* def = catalog.Find(weapon->def);
  if (!def) return 1.0f;
  return 1.0f + (def->ads_fov_scale - 1.0f) * std::clamp(weapon->ads, 0.0f, 1.0f);
}

void StepWeapons(ecs::World& world, physics::PhysicsWorld& physics, const WeaponCatalog& catalog,
                 const HitRegistry& registry, CombatEvents& events, f32 dt) {
  if (!std::isfinite(dt) || dt <= 0) return;

  // Projectile entities are created after the walk: spawning inside World::Each
  // is a structural change that can skip or revisit rows.
  base::Vector<Projectile> spawned;
  // ApplyExplosion walks Health components, so hitscan blasts are deferred until
  // the weapon walk finishes just like projectile creation is.
  base::Vector<ExplosionParams> blasts;

  world.Each<Loadout, WeaponIntent>([&](ecs::Entity entity, Loadout& loadout,
                                        WeaponIntent& intent) {
    const bool reload_requested = intent.reload;
    const i8 switch_requested = intent.switch_to;
    intent.reload = false;
    intent.switch_to = -1;

    if (loadout.count == 0) return;
    if (loadout.active >= loadout.count) loadout.active = 0;

    if (loadout.swap_timer > 0) {
      loadout.swap_timer = std::max(0.0f, loadout.swap_timer - dt);
      if (loadout.swap_timer == 0 && loadout.pending < loadout.count) {
        loadout.active = loadout.pending;
      }
    }

    if (switch_requested >= 0) {
      const u8 slot = static_cast<u8>(switch_requested);
      if (slot < loadout.count && slot != loadout.active) {
        WeaponState& lowered = loadout.slots[loadout.active];
        lowered.reload_timer = 0;
        lowered.reload_shells = false;
        lowered.burst_remaining = 0;
        const WeaponDef* raised = catalog.Find(loadout.slots[slot].def);
        loadout.pending = slot;
        loadout.swap_timer = raised ? std::max(raised->swap_time, 0.0f) : 0.0f;
        if (loadout.swap_timer <= 0) loadout.active = slot;
      }
    }

    WeaponState& weapon = loadout.slots[loadout.active];
    const WeaponDef* def_ptr = catalog.Find(weapon.def);
    if (!def_ptr) {
      weapon.trigger_latched = intent.trigger;
      return;
    }
    const WeaponDef& def = *def_ptr;
    const bool swapping = loadout.swap_timer > 0;

    const f32 ads_target = intent.aim && !swapping ? 1.0f : 0.0f;
    weapon.ads = MoveTowardScalar(weapon.ads, ads_target,
                                  def.ads_time > 0 ? dt / def.ads_time : 1.0f);
    weapon.cooldown = std::max(0.0f, weapon.cooldown - dt);
    weapon.bloom = std::max(0.0f, weapon.bloom - def.spread_decay * dt);

    if (swapping) {
      weapon.burst_remaining = 0;
      weapon.trigger_latched = intent.trigger;
      return;
    }

    if (weapon.reload_timer > 0) {
      weapon.reload_timer -= dt;
      if (weapon.reload_timer <= 0) {
        if (weapon.reload_shells) {
          weapon.ammo += 1;
          weapon.reserve -= 1;
          const bool more = weapon.ammo < def.magazine && weapon.reserve > 0;
          weapon.reload_timer = more ? def.reload_shell_time : 0.0f;
          weapon.reload_shells = more;
        } else {
          CompleteMagazineReload(weapon, def);
        }
      }
    } else if (reload_requested) {
      StartReload(weapon, def);
    }

    bool wants_fire = false;
    if (weapon.burst_remaining > 0) {
      wants_fire = true;  // a burst finishes itself, trigger or not
    } else if (def.mode == FireMode::kAuto) {
      wants_fire = intent.trigger;
    } else {
      wants_fire = intent.trigger && !weapon.trigger_latched;
    }

    // A per-round reload is interruptible: the rounds already in the tube fire.
    if (wants_fire && weapon.reload_shells && weapon.ammo > 0) {
      weapon.reload_timer = 0;
      weapon.reload_shells = false;
    }

    if (!wants_fire || weapon.reload_timer > 0 || weapon.cooldown > kCooldownEpsilon) {
      weapon.trigger_latched = intent.trigger;
      return;
    }

    if (weapon.ammo == 0 && def.magazine > 0) {
      // Dry. Take the pull as a reload request rather than making the player
      // press a second button to hear the same click.
      weapon.burst_remaining = 0;
      StartReload(weapon, def);
      weapon.trigger_latched = intent.trigger;
      return;
    }

    // --- a round leaves ---
    if (def.magazine > 0) --weapon.ammo;
    ++weapon.shots_fired;

    if (def.mode == FireMode::kBurst) {
      if (weapon.burst_remaining == 0) weapon.burst_remaining = std::max(def.burst_count, 1u);
      --weapon.burst_remaining;
    }
    const f32 interval = ShotInterval(def);
    const f32 burst_gap = def.burst_interval > 0 ? def.burst_interval : interval;
    weapon.cooldown = def.mode == FireMode::kBurst && weapon.burst_remaining > 0 ? burst_gap
                                                                                : interval;

    const Vec3 aim = SafeDirection(intent.direction);
    const f32 spread = EffectiveSpread(def, weapon, intent);
    weapon.bloom = std::min(1.0f, weapon.bloom + def.spread_per_shot);

    if (ViewRecoil* recoil = world.Get<ViewRecoil>(entity)) {
      const f32 scale = 1.0f + (def.recoil_ads_scale - 1.0f) * std::clamp(weapon.ads, 0.0f, 1.0f);
      recoil->pending_pitch += def.recoil_pitch * scale;
      recoil->pending_yaw +=
          (def.recoil_yaw + RandomSigned(weapon.rng) * def.recoil_yaw_variance) * scale;
      recoil->time_since_shot = 0;
    }
    if (Viewmodel* viewmodel = world.Get<Viewmodel>(entity)) {
      viewmodel->punch = std::min(viewmodel->punch + viewmodel->punch_scale,
                                  viewmodel->punch_scale * 3.0f);
    }

    ShotEvent shot;
    shot.shooter = entity;
    shot.weapon = weapon.def;
    shot.origin = intent.origin;
    shot.direction = aim;
    shot.pellets = std::max(def.pellets, 1u);
    shot.ammo_left = weapon.ammo;
    events.shots.push_back(shot);

    physics::BodyId ignore[kMaxIgnoredBodies];
    u32 ignore_count = 0;
    if (const HitIgnoreList* list = world.Get<HitIgnoreList>(entity)) {
      const u8 count = std::min<u8>(list->count, kMaxIgnoredBodies);
      for (u8 i = 0; i < count; ++i) {
        if (list->bodies[i] != 0) ignore[ignore_count++] = list->bodies[i];
      }
    }

    ShotContext context;
    context.world = &world;
    context.physics = &physics;
    context.registry = &registry;
    context.events = &events;
    context.blasts = &blasts;
    context.shooter = entity;
    context.ignore = ignore_count > 0 ? ignore : nullptr;
    context.ignore_count = ignore_count;

    for (u32 pellet = 0; pellet < shot.pellets; ++pellet) {
      const Vec3 direction = ConeDirection(aim, spread, weapon.rng);
      if (def.kind == WeaponKind::kHitscan) {
        ResolveHitscan(context, def, intent.origin, direction);
        continue;
      }
      Projectile round;
      round.owner = entity;
      round.def = weapon.def;
      round.source = def.name_hash;
      round.position = intent.origin;
      round.velocity = direction * def.muzzle_speed;
      round.damage = def.damage;
      round.impulse = def.impulse;
      round.gravity = def.projectile_gravity;
      round.drag = def.projectile_drag;
      round.radius = def.projectile_radius;
      round.life = def.projectile_life;
      round.blast_radius = def.blast_radius;
      round.blast_damage = def.blast_damage;
      round.blast_min_scale = def.blast_min_scale;
      round.blast_impulse = def.blast_impulse;
      round.explode_on_expire = def.explode_on_expire;
      round.ignore_count = static_cast<u8>(std::min<u32>(ignore_count, kMaxIgnoredBodies));
      for (u8 i = 0; i < round.ignore_count; ++i) round.ignore[i] = ignore[i];
      spawned.push_back(round);
    }

    weapon.trigger_latched = intent.trigger;
  });

  for (const Projectile& round : spawned) SpawnProjectile(world, round);
  for (const ExplosionParams& blast : blasts) {
    ApplyExplosion(world, physics, registry, blast, &events);
  }
}

void StepViewRecoil(ecs::World& world, f32 dt) {
  if (!std::isfinite(dt) || dt <= 0) return;
  world.Each<ViewRecoil>([dt](ecs::Entity, ViewRecoil& recoil) {
    recoil.time_since_shot += dt;
    f32 pitch = 0;
    f32 yaw = 0;

    const f32 kick = ApproachFraction(recoil.kick_half_life, dt);
    const f32 kick_pitch = recoil.pending_pitch * kick;
    const f32 kick_yaw = recoil.pending_yaw * kick;
    recoil.pending_pitch -= kick_pitch;
    recoil.pending_yaw -= kick_yaw;
    pitch += kick_pitch;
    yaw += kick_yaw;

    const f32 keep = std::clamp(recoil.recovery_fraction, 0.0f, 1.0f);
    recoil.recoverable_pitch += kick_pitch * keep;
    recoil.recoverable_yaw += kick_yaw * keep;

    if (recoil.time_since_shot >= recoil.recovery_delay) {
      const f32 back = ApproachFraction(recoil.recovery_half_life, dt);
      const f32 back_pitch = recoil.recoverable_pitch * back;
      const f32 back_yaw = recoil.recoverable_yaw * back;
      recoil.recoverable_pitch -= back_pitch;
      recoil.recoverable_yaw -= back_yaw;
      pitch -= back_pitch;
      yaw -= back_yaw;
    }

    recoil.view_pitch_delta = pitch;
    recoil.view_yaw_delta = yaw;
  });
}

void StepViewmodels(ecs::World& world, f32 dt) {
  if (!std::isfinite(dt) || dt <= 0) return;
  world.Each<Viewmodel, WeaponIntent>([&](ecs::Entity entity, Viewmodel& viewmodel,
                                          WeaponIntent& intent) {
    // Sway trails the look RATE, so a flick throws the weapon and holding still
    // settles it back to centre.
    const f32 yaw_rate = intent.look_yaw_delta / dt;
    const f32 pitch_rate = intent.look_pitch_delta / dt;
    const f32 sway_x =
        std::clamp(-yaw_rate * viewmodel.sway_scale, -viewmodel.sway_max, viewmodel.sway_max);
    const f32 sway_y =
        std::clamp(-pitch_rate * viewmodel.sway_scale, -viewmodel.sway_max, viewmodel.sway_max);
    const f32 sway_blend = ApproachFraction(viewmodel.sway_half_life, dt);

    const f32 speed_ratio =
        viewmodel.bob_reference_speed > 0
            ? std::clamp(intent.speed / viewmodel.bob_reference_speed, 0.0f, 1.0f)
            : 0.0f;
    const f32 bob_target = intent.airborne ? 0.0f : speed_ratio;
    viewmodel.bob_weight +=
        (bob_target - viewmodel.bob_weight) * ApproachFraction(viewmodel.bob_half_life, dt);
    viewmodel.bob_phase =
        std::fmod(viewmodel.bob_phase + viewmodel.bob_rate * speed_ratio * dt, kTwoPi);

    viewmodel.punch -= viewmodel.punch * ApproachFraction(viewmodel.punch_half_life, dt);

    f32 ads = 0;
    if (const Loadout* loadout = world.Get<Loadout>(entity)) {
      if (const WeaponState* weapon = ActiveWeapon(*loadout)) {
        ads = std::clamp(weapon->ads, 0.0f, 1.0f);
      }
    }
    // Aiming pulls the weapon to the eye line: sights are steady, so bob and
    // sway fade out with the same blend that moves it there.
    const f32 hip = 1.0f - ads;

    const f32 bob = viewmodel.bob_amplitude * viewmodel.bob_weight * hip;
    Vec3 target;
    target.x = sway_x * hip + std::sin(viewmodel.bob_phase) * bob;
    target.y = sway_y * hip + std::sin(viewmodel.bob_phase * 2.0f) * bob * 0.5f;
    target.z = -viewmodel.punch;
    target += viewmodel.ads_offset * ads;

    viewmodel.offset += (target - viewmodel.offset) * sway_blend;
    viewmodel.roll = -viewmodel.offset.x * viewmodel.roll_scale;
    viewmodel.yaw = -viewmodel.offset.x * 0.5f;
    viewmodel.pitch = -viewmodel.offset.y * 0.5f;
  });
}

}  // namespace rx::combat
