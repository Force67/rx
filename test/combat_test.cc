#include <cmath>
#include <cstdio>

#include "combat/damage.h"
#include "combat/projectile.h"
#include "combat/weapon.h"
#include "ecs/world.h"
#include "physics/physics_world.h"
#include "scene/components.h"

using namespace rx;
using namespace rx::combat;
namespace ecs = rx::ecs;
namespace scene = rx::scene;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
int failures = 0;

void Check(bool condition, const char* message) {
  if (condition) return;
  std::fprintf(stderr, "combat_test: FAIL: %s\n", message);
  ++failures;
}

void Near(f32 actual, f32 expected, const char* message, f32 epsilon = 1e-3f) {
  if (std::abs(actual - expected) <= epsilon) return;
  std::fprintf(stderr, "combat_test: FAIL: %s (got %.4f, expected %.4f)\n", message, actual,
               expected);
  ++failures;
}

// A shooter at the origin looking down -Z, a target 10 m in front of it with a
// head box on top of a torso box, and a floor. Everything the systems need is
// wired the way a game would wire it.
struct Range {
  physics::PhysicsWorld physics;
  ecs::World world;
  WeaponCatalog catalog;
  HitRegistry registry;
  CombatEvents events;

  ecs::Entity shooter{};
  ecs::Entity target{};
  physics::BodyId torso_body = 0;
  physics::BodyId head_body = 0;

  bool Init() {
    if (!physics.Initialize()) return false;
    physics.AddStaticBox({0, -0.5f, 0}, {80, 0.5f, 80});

    shooter = world.Create();
    world.Add(shooter, Loadout{});
    world.Add(shooter, WeaponIntent{});
    world.Add(shooter, ViewRecoil{});
    world.Add(shooter, Team{1, false});

    target = world.Create();
    Health health;
    health.hp = 100;
    health.max_hp = 100;
    world.Add(target, health);
    world.Add(target, Damageable{});
    world.Add(target, Team{2, false});
    scene::Transform transform;
    transform.position[2] = -10.0f;
    world.Add(target, transform);

    // Kinematic hitboxes: a character controller has no body of its own, so the
    // game gives it one per zone and registers them.
    torso_body = physics.AddKinematicBox({0, 1.0f, -10.0f}, {0.35f, 0.6f, 0.25f});
    head_body = physics.AddKinematicBox({0, 1.75f, -10.0f}, {0.14f, 0.14f, 0.14f});
    registry.Register(torso_body, target, HitZone::kTorso);
    registry.Register(head_body, target, HitZone::kHead);

    physics.Update(kDt);  // build the broadphase before the first query
    return true;
  }

  WeaponIntent& intent() { return *world.Get<WeaponIntent>(shooter); }
  Loadout& loadout() { return *world.Get<Loadout>(shooter); }
  WeaponState& weapon() { return *ActiveWeapon(loadout()); }
  Health& health() { return *world.Get<Health>(target); }

  void Aim(const Vec3& origin, const Vec3& direction) {
    intent().origin = origin;
    intent().direction = direction;
  }

  void Step(int count) {
    for (int i = 0; i < count; ++i) {
      StepWeapons(world, physics, catalog, registry, events, kDt);
      StepProjectiles(world, physics, registry, events, kDt);
      StepViewRecoil(world, kDt);
      StepHealth(world, kDt);
      physics.Update(kDt);
    }
  }
};

WeaponDef Rifle() {
  WeaponDef def;
  def.name_hash = 0x1111;
  def.mode = FireMode::kAuto;
  def.rpm = 600.0f;  // one round every 0.1 s
  def.damage = 25.0f;
  def.magazine = 10;
  def.reserve_max = 90;
  def.reload_time = 2.0f;
  def.reload_empty_time = 2.5f;
  def.spread_min = 0;  // deterministic aim for the hit tests
  def.spread_max = 0;
  def.spread_per_shot = 0.25f;
  def.spread_decay = 1.0f;
  def.recoil_pitch = 0.02f;
  def.recoil_yaw = 0;
  def.recoil_yaw_variance = 0;
  def.ads_time = 0.2f;
  def.swap_time = 0.4f;
  return def;
}

void TestFireRateAndAmmo() {
  Range range;
  if (!range.Init()) return;
  const WeaponDefId rifle = range.catalog.Register(Rifle());
  Check(GiveWeapon(range.loadout(), range.catalog, rifle, 30) == 0, "rifle lands in slot 0");
  range.Aim({0, 1.0f, 0}, {0, 0, -1});
  range.intent().trigger = true;

  range.Step(1);
  Check(range.events.shots.size() == 1, "the first pull fires immediately");
  Check(range.weapon().ammo == 9, "the round came out of the magazine");

  // 600 rpm is 0.1 s between rounds, which is exactly six 1/60 s steps.
  range.Step(5);
  Check(range.events.shots.size() == 1, "the rate of fire holds the next round back");
  range.Step(1);
  Check(range.events.shots.size() == 2, "and lets it go on time");

  range.Step(60);
  Check(range.weapon().ammo == 0, "the magazine runs dry");
  Check(range.weapon().reload_timer > 0, "a dry trigger pull starts the reload");
  Check(range.weapon().reload_empty, "the reload knows it started empty");
}

void TestSemiAndBurst() {
  Range range;
  if (!range.Init()) return;
  WeaponDef pistol = Rifle();
  pistol.mode = FireMode::kSemi;
  pistol.rpm = 1200.0f;
  const WeaponDefId semi = range.catalog.Register(pistol);
  GiveWeapon(range.loadout(), range.catalog, semi, 30);
  range.Aim({0, 1.0f, 0}, {0, 0, -1});
  range.intent().trigger = true;

  range.Step(30);
  Check(range.events.shots.size() == 1, "semi-auto fires once per pull");
  range.intent().trigger = false;
  range.Step(1);
  range.intent().trigger = true;
  range.Step(1);
  Check(range.events.shots.size() == 2, "releasing the trigger re-arms it");

  Range burst_range;
  if (!burst_range.Init()) return;
  WeaponDef burst_def = Rifle();
  burst_def.mode = FireMode::kBurst;
  burst_def.burst_count = 3;
  burst_def.burst_interval = 0.05f;
  const WeaponDefId burst = burst_range.catalog.Register(burst_def);
  GiveWeapon(burst_range.loadout(), burst_range.catalog, burst, 30);
  burst_range.Aim({0, 1.0f, 0}, {0, 0, -1});
  burst_range.intent().trigger = true;
  burst_range.Step(1);
  burst_range.intent().trigger = false;  // let go: the burst still finishes
  burst_range.Step(30);
  Check(burst_range.events.shots.size() == 3, "a burst completes after the trigger is released");
}

void TestZonesAndFalloff() {
  Range range;
  if (!range.Init()) return;
  WeaponDef def = Rifle();
  def.falloff_start = 5.0f;
  def.falloff_end = 25.0f;
  def.falloff_min_scale = 0.5f;
  const WeaponDefId rifle = range.catalog.Register(def);
  GiveWeapon(range.loadout(), range.catalog, rifle, 30);

  Near(FalloffScale(def, 2.0f), 1.0f, "no falloff inside the near band");
  Near(FalloffScale(def, 15.0f), 0.75f, "falloff is linear across the band");
  Near(FalloffScale(def, 40.0f), 0.5f, "falloff floors at the far scale");

  // The torso box is 0.25 m deep, so its face is at 9.75 m: 25 damage scaled by
  // the falloff there, times the 1.0 torso zone.
  const f32 torso_scale = FalloffScale(def, 9.75f);
  range.Aim({0, 1.0f, 0}, {0, 0, -1});
  range.intent().trigger = true;
  range.Step(1);
  Check(range.events.damage.size() == 1, "the torso shot registered");
  Near(range.health().hp, 100.0f - 25.0f * torso_scale, "falloff scales the torso hit", 0.05f);

  // The head box sits 0.14 m proud of the torso, doubled by the zone table.
  const f32 before = range.health().hp;
  range.intent().trigger = false;
  range.Step(1);
  range.Aim({0, 1.75f, 0}, {0, 0, -1});
  range.intent().trigger = true;
  range.Step(6);  // let the rate of fire come round again
  Near(before - range.health().hp, 2.0f * 25.0f * FalloffScale(def, 9.86f),
       "a head shot doubles", 0.05f);
}

void TestArmorAndRegen() {
  ecs::World world;
  CombatEvents events;
  const ecs::Entity entity = world.Create();
  Health health;
  health.hp = 100;
  health.max_hp = 100;
  health.armor = 50;
  health.armor_absorb = 0.5f;
  health.regen_delay = 1.0f;
  health.regen_rate = 20.0f;
  world.Add(entity, health);

  DamageRequest request;
  request.target = entity;
  request.amount = 40.0f;
  Check(ApplyDamage(world, request, &events), "damage landed");
  Near(world.Get<Health>(entity)->armor, 30.0f, "armor took its half");
  Near(world.Get<Health>(entity)->hp, 80.0f, "the rest came off health");

  for (int i = 0; i < 30; ++i) StepHealth(world, kDt);  // 0.5 s: still in the delay
  Near(world.Get<Health>(entity)->hp, 80.0f, "regen waits out the delay");
  for (int i = 0; i < 90; ++i) StepHealth(world, kDt);  // 1.5 s more, 1 s of it regening
  Near(world.Get<Health>(entity)->hp, 100.0f, "regen tops back up and stops at max");

  DamageRequest lethal = request;
  lethal.amount = 500.0f;
  ApplyDamage(world, lethal, &events);
  Check(world.Get<Health>(entity)->dead, "an overkill hit kills");
  Check(!ApplyDamage(world, request, &events), "the dead take no more damage");
}

void TestFriendlyFire() {
  Range range;
  if (!range.Init()) return;
  range.world.Get<Team>(range.target)->id = 1;  // same team as the shooter
  const WeaponDefId rifle = range.catalog.Register(Rifle());
  GiveWeapon(range.loadout(), range.catalog, rifle, 30);
  range.Aim({0, 1.0f, 0}, {0, 0, -1});
  range.intent().trigger = true;
  range.Step(1);
  Check(range.events.impacts.size() == 1, "the round still hits the team-mate");
  Near(range.health().hp, 100.0f, "but friendly fire is off, so nothing lands");

  // The shooter's rules of engagement decide, not the victim's.
  range.world.Get<Team>(range.shooter)->friendly_fire = true;
  range.Step(10);
  Check(range.health().hp < 100.0f, "turning friendly fire on lets it through");
}

void TestReloadAndSwitch() {
  Range range;
  if (!range.Init()) return;
  const WeaponDefId rifle = range.catalog.Register(Rifle());
  WeaponDef shotgun_def = Rifle();
  shotgun_def.name_hash = 0x2222;
  shotgun_def.magazine = 4;
  shotgun_def.reload_shell_time = 0.4f;
  shotgun_def.swap_time = 0.5f;
  const WeaponDefId shotgun = range.catalog.Register(shotgun_def);

  GiveWeapon(range.loadout(), range.catalog, rifle, 30);
  GiveWeapon(range.loadout(), range.catalog, shotgun, 20);
  range.Aim({0, 1.0f, 0}, {0, 0, -1});

  // Magazine reload: fire three, reload, get exactly those three back.
  range.intent().trigger = true;
  range.Step(13);
  range.intent().trigger = false;
  const u32 fired = 10 - range.weapon().ammo;
  Check(fired == 3, "three rounds left the rifle");
  range.intent().reload = true;
  range.Step(1);
  Check(range.weapon().reload_timer > 0, "the reload started");
  range.Step(130);  // > 2 s
  Check(range.weapon().ammo == 10, "the magazine is full again");
  Check(range.weapon().reserve == 30 - fired, "the rounds came out of the reserve");

  // Switching takes swap_time and then raises the other weapon.
  range.intent().switch_to = 1;
  range.Step(1);
  Check(range.loadout().active == 0, "the swap is still in flight");
  Check(range.loadout().swap_timer > 0, "the swap timer is running");
  range.Step(40);
  Check(range.loadout().active == 1, "the shotgun came up");

  // Per-round reload: each shell is a separate step, and firing interrupts it.
  range.intent().trigger = true;
  range.Step(60);
  range.intent().trigger = false;
  range.Step(1);
  const u32 shells_left = range.weapon().ammo;
  Check(shells_left < 4, "the shotgun fired");
  range.intent().reload = true;
  range.Step(1);
  range.Step(25);  // one shell period
  Check(range.weapon().ammo == shells_left + 1, "one shell went in");
  Check(range.weapon().reload_shells, "and it kept going for the next");
}

void TestSpreadBloomAndAds() {
  WeaponDef def = Rifle();
  def.spread_min = 0.002f;
  def.spread_max = 0.05f;
  def.spread_ads_scale = 0.25f;
  def.spread_move_scale = 2.0f;
  def.spread_move_speed = 4.0f;

  WeaponState weapon;
  WeaponIntent intent;
  Near(EffectiveSpread(def, weapon, intent), 0.002f, "a settled weapon shoots its minimum");

  weapon.bloom = 1.0f;
  Near(EffectiveSpread(def, weapon, intent), 0.05f, "full bloom shoots the maximum");

  weapon.bloom = 0;
  intent.speed = 4.0f;
  Near(EffectiveSpread(def, weapon, intent), 0.004f, "running doubles the cone");

  intent.speed = 0;
  intent.airborne = true;
  Near(EffectiveSpread(def, weapon, intent), 0.002f * def.spread_air_scale,
       "jumping scatters it");

  intent.airborne = false;
  weapon.ads = 1.0f;
  Near(EffectiveSpread(def, weapon, intent), 0.0005f, "aiming tightens it");

  Range range;
  if (!range.Init()) return;
  const WeaponDefId rifle = range.catalog.Register(def);
  GiveWeapon(range.loadout(), range.catalog, rifle, 30);
  range.Aim({0, 1.0f, 0}, {0, 0, -1});
  range.intent().aim = true;
  range.Step(20);  // ads_time is 0.2 s
  Near(range.weapon().ads, 1.0f, "the sights come up");
  Near(AimFovScale(range.loadout(), range.catalog), def.ads_fov_scale, "and the fov zooms");

  range.intent().trigger = true;
  range.Step(1);
  Check(range.weapon().bloom > 0, "firing blooms the cone");
  range.intent().trigger = false;
  range.Step(120);
  Near(range.weapon().bloom, 0.0f, "and the bloom bleeds off");
}

void TestRecoilKickAndRecovery() {
  Range range;
  if (!range.Init()) return;
  const WeaponDefId rifle = range.catalog.Register(Rifle());
  GiveWeapon(range.loadout(), range.catalog, rifle, 30);
  range.Aim({0, 1.0f, 0}, {0, 0, -1});

  ViewRecoil& recoil = *range.world.Get<ViewRecoil>(range.shooter);
  // Hold the recovery off long enough to watch the two phases separately.
  recoil.recovery_delay = 0.5f;

  range.intent().trigger = true;
  range.Step(1);
  range.intent().trigger = false;

  Check(recoil.view_pitch_delta > 0, "the shot kicks the view up");

  // Net view rotation, summed the way a game would apply it.
  f32 view = recoil.view_pitch_delta;
  for (int i = 0; i < 20; ++i) {  // 0.33 s, still inside the recovery delay
    range.Step(1);
    view += recoil.view_pitch_delta;
  }
  Near(view, 0.02f, "the whole kick reaches the view", 5e-4f);

  for (int i = 0; i < 120; ++i) {
    range.Step(1);
    view += recoil.view_pitch_delta;
  }
  // recovery_fraction is 0.75, so a quarter of the kick stays up for the player
  // to pull back down.
  Near(view, 0.005f, "three quarters of it comes back on its own", 5e-4f);
}

void TestPenetration() {
  Range range;
  if (!range.Init()) return;
  // A thin plank at 5 m and a thick block at 6 m, both between the muzzle and
  // the target. The round should punch the plank and stop in the block.
  range.physics.AddStaticBox({0, 1.0f, -5.0f}, {2.0f, 2.0f, 0.05f});
  range.physics.Update(kDt);

  WeaponDef def = Rifle();
  def.max_penetrations = 1;
  def.penetration = 0.2f;
  def.penetration_damage_scale = 0.5f;
  const WeaponDefId rifle = range.catalog.Register(def);
  GiveWeapon(range.loadout(), range.catalog, rifle, 30);
  range.Aim({0, 1.0f, 0}, {0, 0, -1});
  range.intent().trigger = true;
  range.Step(1);

  Check(range.events.impacts.size() == 2, "the round marked the plank and the torso behind it");
  Check(range.events.impacts[0].penetrated, "the plank was flagged as penetrated");
  Near(range.health().hp, 100.0f - 12.5f, "the target took half damage through cover", 0.05f);

  // A wall thicker than the budget stops the round dead.
  Range thick;
  if (!thick.Init()) return;
  thick.physics.AddStaticBox({0, 1.0f, -5.0f}, {2.0f, 2.0f, 1.0f});
  thick.physics.Update(kDt);
  const WeaponDefId same = thick.catalog.Register(def);
  GiveWeapon(thick.loadout(), thick.catalog, same, 30);
  thick.Aim({0, 1.0f, 0}, {0, 0, -1});
  thick.intent().trigger = true;
  thick.Step(1);
  Check(thick.events.impacts.size() == 1, "a thick wall is cover");
  Near(thick.health().hp, 100.0f, "and nothing reaches the target");
}

void TestProjectileBallistics() {
  Range range;
  if (!range.Init()) return;
  WeaponDef def = Rifle();
  def.kind = WeaponKind::kProjectile;
  def.mode = FireMode::kSemi;
  def.damage = 40.0f;
  def.muzzle_speed = 20.0f;
  def.projectile_gravity = 9.81f;
  def.projectile_life = 5.0f;
  def.spread_min = 0;
  def.spread_max = 0;
  const WeaponDefId launcher = range.catalog.Register(def);
  GiveWeapon(range.loadout(), range.catalog, launcher, 30);

  // Flat at the torso from 10 m: at 20 m/s the round needs half a second and
  // drops ~1.2 m on the way, so a flat shot lands short.
  range.Aim({0, 1.0f, 0}, {0, 0, -1});
  range.intent().trigger = true;
  range.Step(1);
  range.intent().trigger = false;
  Check(range.events.shots.size() == 1, "the launcher fired");

  int steps = 0;
  while (range.events.damage.empty() && steps < 120) {
    range.Step(1);
    ++steps;
  }
  Check(range.events.damage.empty(), "a flat shot at 20 m/s drops short of the torso");

  // Lead it upward and it arrives.
  Range lofted;
  if (!lofted.Init()) return;
  const WeaponDefId same = lofted.catalog.Register(def);
  GiveWeapon(lofted.loadout(), lofted.catalog, same, 30);
  lofted.Aim({0, 1.0f, 0}, Normalize(Vec3{0, 0.13f, -1}));
  lofted.intent().trigger = true;
  lofted.Step(1);
  lofted.intent().trigger = false;
  steps = 0;
  while (lofted.events.damage.empty() && steps < 120) {
    lofted.Step(1);
    ++steps;
  }
  Check(!lofted.events.damage.empty(), "an arced shot connects");
  Check(lofted.health().hp < 100.0f, "and the target takes it");
}

void TestExplosionFalloffAndCover() {
  Range range;
  if (!range.Init()) return;

  ExplosionParams blast;
  blast.position = {0, 1.0f, -12.0f};  // 2 m from the target
  blast.radius = 4.0f;
  blast.damage = 100.0f;
  blast.min_scale = 0.0f;
  blast.line_of_sight = false;
  Check(ApplyExplosion(range.world, range.physics, range.registry, blast, &range.events) == 1,
        "the blast caught the target");
  Near(range.health().hp, 50.0f, "linear falloff halved it at half the radius", 0.5f);

  // Out of range entirely.
  range.health().hp = 100.0f;
  blast.position = {0, 1.0f, -20.0f};
  Check(ApplyExplosion(range.world, range.physics, range.registry, blast, &range.events) == 0,
        "nothing outside the radius is touched");

  // A wall on the far side of the target, and a blast behind it.
  range.physics.AddStaticBox({0, 1.0f, -11.0f}, {3.0f, 3.0f, 0.2f});
  range.physics.Update(kDt);
  range.health().hp = 100.0f;
  blast.position = {0, 1.0f, -12.0f};
  blast.line_of_sight = false;
  Check(ApplyExplosion(range.world, range.physics, range.registry, blast, &range.events) == 1,
        "the wall does not matter when line of sight is off");

  range.health().hp = 100.0f;
  blast.line_of_sight = true;
  Check(ApplyExplosion(range.world, range.physics, range.registry, blast, &range.events) == 0,
        "with it on, the wall is cover");
  Near(range.health().hp, 100.0f, "and the blast does not reach through");

  range.health().hp = 100.0f;
  blast.position = {0, 1.0f, -9.0f};  // open side, 1 m from the target
  ApplyExplosion(range.world, range.physics, range.registry, blast, &range.events);
  Near(range.health().hp, 100.0f - 75.0f, "line of sight through open air still lands", 1.0f);
}

void TestHitRegistryLifetime() {
  ecs::World world;
  HitRegistry registry;
  const ecs::Entity a = world.Create();
  const ecs::Entity b = world.Create();
  registry.Register(1, a, HitZone::kHead);
  registry.Register(2, a, HitZone::kTorso);
  registry.Register(3, b);
  Check(registry.size() == 3, "three bodies registered");
  Check(registry.Find(1) != nullptr && registry.Find(1)->zone == HitZone::kHead,
        "a body resolves to its zone");
  Check(registry.UnregisterTarget(a) == 2, "dropping a target drops all its bodies");
  Check(registry.Find(1) == nullptr, "and they stop resolving");
  Check(registry.Find(3) != nullptr, "without touching anyone else");
}

}  // namespace

int main() {
  TestFireRateAndAmmo();
  TestSemiAndBurst();
  TestZonesAndFalloff();
  TestArmorAndRegen();
  TestFriendlyFire();
  TestReloadAndSwitch();
  TestSpreadBloomAndAds();
  TestRecoilKickAndRecovery();
  TestPenetration();
  TestProjectileBallistics();
  TestExplosionFalloffAndCover();
  TestHitRegistryLifetime();

  if (failures == 0) {
    std::printf("combat_test: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "combat_test: %d checks failed\n", failures);
  return 1;
}
