#ifndef RX_COMBAT_WEAPON_DEF_H_
#define RX_COMBAT_WEAPON_DEF_H_

#include <base/containers/unordered_map.h>

#include "core/export.h"
#include "core/types.h"

// Weapon definitions and the game-owned table they live in. A definition is
// immutable tuning data; everything mutable (ammo, heat of the moment) lives in
// the WeaponState component. The engine assigns no taxonomy: there is no
// "pistol" or "rocket launcher" here, only the mechanics a shooter is built
// from, so a game describes its arsenal as data.
namespace rx::combat {

using WeaponDefId = u32;
constexpr WeaponDefId kInvalidWeaponDef = 0;

// What the trigger does while held.
enum class FireMode : u8 {
  kSemi,   // one round per pull; the trigger must be released to fire again
  kAuto,   // rounds keep leaving while held, paced by `rpm`
  kBurst,  // one pull fires `burst_count` rounds paced by `burst_interval`
};

// How a round travels. Hitscan resolves instantly along a ray; projectiles are
// entities with velocity, drag and gravity, so they can be led and dropped.
enum class WeaponKind : u8 { kHitscan, kProjectile };

struct WeaponDef {
  u64 name_hash = 0;  // game string hash (display / debug / identity)

  WeaponKind kind = WeaponKind::kHitscan;
  FireMode mode = FireMode::kSemi;

  // --- rate of fire ---------------------------------------------------------
  f32 rpm = 600.0f;         // rounds per minute between trigger events
  u32 burst_count = 3;      // rounds per burst (kBurst only)
  f32 burst_interval = 0;   // s between rounds INSIDE a burst; 0 uses the rpm gap
  u32 pellets = 1;          // rounds released per shot; >1 is a shotgun spread

  // --- damage ---------------------------------------------------------------
  f32 damage = 25.0f;  // per pellet, before falloff, zone and armor scaling
  f32 range = 200.0f;  // metres a hitscan ray travels before it gives up
  // Linear damage falloff between the two distances, flooring at
  // `falloff_min_scale`. Equal distances (the default) disable falloff.
  f32 falloff_start = 0;
  f32 falloff_end = 0;
  f32 falloff_min_scale = 0.5f;

  // --- penetration ----------------------------------------------------------
  // A hitscan round may punch through up to `max_penetrations` surfaces. After
  // each one the ray resumes `penetration` metres further along, so the field is
  // a thickness budget: anything thicker than that is effectively cover, because
  // the resumed ray restarts inside it and hits it again. Damage is scaled by
  // `penetration_damage_scale` per surface crossed. 0 penetrations = every
  // surface stops the round.
  u32 max_penetrations = 0;
  f32 penetration = 0.15f;
  f32 penetration_damage_scale = 0.6f;

  f32 impulse = 0;  // N*s pushed into a dynamic body along the shot direction

  // --- accuracy -------------------------------------------------------------
  // Shots leave inside a cone whose half-angle is lerped from `spread_min` to
  // `spread_max` by the bloom the weapon has accumulated. Every shot adds
  // `spread_per_shot` (in bloom units, 1 = fully bloomed) and bloom bleeds off
  // at `spread_decay` per second. The scales below multiply the resulting angle.
  f32 spread_min = 0.002f;     // rad, first-shot cone half-angle
  f32 spread_max = 0.05f;      // rad, fully bloomed
  f32 spread_per_shot = 0.18f;
  f32 spread_decay = 1.4f;     // bloom units per second
  f32 spread_move_scale = 2.2f;   // multiplier at `spread_move_speed` and above
  f32 spread_move_speed = 4.0f;   // m/s at which the move penalty is fully applied
  f32 spread_air_scale = 3.0f;    // multiplier while airborne
  f32 spread_crouch_scale = 0.7f; // multiplier while crouched
  f32 spread_ads_scale = 0.25f;   // multiplier at full aim-down-sights

  // --- recoil ---------------------------------------------------------------
  // Per shot view kick, fed to the entity's ViewRecoil (see components.h).
  f32 recoil_pitch = 0.012f;      // rad up per shot
  f32 recoil_yaw = 0.003f;        // rad of horizontal bias per shot (signed)
  f32 recoil_yaw_variance = 0.004f;  // rad of random horizontal jitter
  f32 recoil_ads_scale = 0.6f;    // kick multiplier at full aim-down-sights

  // --- ammunition -----------------------------------------------------------
  u32 magazine = 30;      // rounds per magazine; 0 means the weapon never runs dry
  u32 reserve_max = 240;  // cap on carried rounds; 0 = uncapped
  f32 reload_time = 2.1f;         // s for a magazine swap with a round chambered
  f32 reload_empty_time = 2.6f;   // s from empty (bolt release); 0 uses reload_time
  // Per-round reloading (shotguns): >0 makes a reload push a single round every
  // `reload_shell_time` seconds until the magazine is full, and firing or
  // switching interrupts it after the rounds already loaded.
  f32 reload_shell_time = 0;

  // --- handling -------------------------------------------------------------
  f32 ads_time = 0.22f;   // s from hip to fully aimed (and back)
  f32 ads_fov_scale = 0.75f;  // camera fov multiplier at full aim; 1 = no zoom
  f32 swap_time = 0.5f;   // s to bring this weapon up when switched to

  // --- projectile (kind == kProjectile) -------------------------------------
  f32 muzzle_speed = 40.0f;      // m/s
  f32 projectile_gravity = 9.81f;  // m/s^2 down
  f32 projectile_drag = 0;       // 1/m: velocity loses this fraction per metre
  f32 projectile_radius = 0;     // >0 sweeps a sphere instead of a ray
  f32 projectile_life = 8.0f;    // s before it despawns (or detonates, below)
  bool explode_on_expire = false;  // timed frag: detonate at the end of life

  // --- explosion (both kinds; 0 radius = no blast) --------------------------
  f32 blast_radius = 0;
  f32 blast_damage = 0;
  f32 blast_min_scale = 0.15f;  // damage scale at the rim of the radius
  f32 blast_impulse = 0;        // N*s at the centre, falling off with the damage

  u32 flags = 0;    // game-defined bits
  u64 payload = 0;  // game-defined per-definition data
};

// A game-owned table of WeaponDefs, passed explicitly to the systems that need
// definition data. There is no global catalog, for the same reason
// inventory::ItemCatalog has none: a game may hold several and choose.
class RX_COMBAT_EXPORT WeaponCatalog {
 public:
  // Registers `def` under a freshly minted id and returns it.
  WeaponDefId Register(const WeaponDef& def);
  // Registers `def` under an explicit id (data-driven / save-stable catalogs).
  // Overwrites any existing entry. Returns `id`.
  WeaponDefId Register(WeaponDefId id, const WeaponDef& def);
  // Definition for `id`, or nullptr if unknown (id 0 is always unknown). The
  // pointer stays valid until that entry is removed or the catalog dies.
  const WeaponDef* Find(WeaponDefId id) const;
  bool Contains(WeaponDefId id) const { return Find(id) != nullptr; }
  void Remove(WeaponDefId id);
  void Clear();
  mem_size size() const { return defs_.size(); }

 private:
  base::UnorderedMap<WeaponDefId, WeaponDef> defs_;
  WeaponDefId next_id_ = 1;
};

// Damage scale at `distance` for `def`'s falloff curve.
RX_COMBAT_EXPORT f32 FalloffScale(const WeaponDef& def, f32 distance);

// Seconds between two rounds leaving the barrel at `def`'s rate of fire.
RX_COMBAT_EXPORT f32 ShotInterval(const WeaponDef& def);

}  // namespace rx::combat

#endif  // RX_COMBAT_WEAPON_DEF_H_
