#ifndef RX_COMBAT_COMPONENTS_H_
#define RX_COMBAT_COMPONENTS_H_

#include "combat/weapon_def.h"
#include "core/math.h"
#include "core/types.h"
#include "ecs/entity.h"

// Plain-data components for the shooter systems. Everything here is POD: no
// component owns a heap allocation, so the archetype storage relocates them by
// memcpy and a save system can blit them.
namespace rx::combat {

// Where a shot landed on a target. The engine attaches no anatomy to these:
// they are four indices into a per-target multiplier table, and a game decides
// that index 1 means "head" by registering its head hitbox with kHead.
enum class HitZone : u8 { kDefault, kHead, kTorso, kLimb };
constexpr u32 kHitZoneCount = 4;

// --- health ---------------------------------------------------------------

// Attach to anything that can be shot. Armor soaks `armor_absorb` of every
// incoming hit until it runs out. Regeneration is opt-in: `regen_rate` 0 (the
// default) leaves health where the last hit left it.
struct Health {
  f32 hp = 100.0f;
  f32 max_hp = 100.0f;
  f32 armor = 0;
  f32 max_armor = 0;
  f32 armor_absorb = 0.5f;  // [0..1] of each hit taken by armor while it lasts
  f32 regen_delay = 0;      // s without damage before regeneration starts
  f32 regen_rate = 0;       // hp per second once it starts; 0 disables regen
  f32 time_since_damage = 0;
  bool dead = false;
};

// Optional tuning beside a Health. Absent means "every zone counts 1x".
struct Damageable {
  // Indexed by HitZone. The defaults are the classic shooter curve: a head shot
  // doubles, a limb shot is discounted.
  f32 zone_multiplier[kHitZoneCount] = {1.0f, 2.0f, 1.0f, 0.75f};
  f32 damage_scale = 1.0f;   // global multiplier (difficulty, buffs, armor kits)
  bool invulnerable = false;
  // Where the entity's mass sits relative to its Transform, for explosion
  // distance and line-of-sight. A character Transform is at the feet, so the
  // default lifts the sample to chest height.
  Vec3 center_offset = {0, 1.0f, 0};
};

// Optional. Two entities on the same team only damage each other when the
// instigator's team allows it.
struct Team {
  u32 id = 0;
  bool friendly_fire = false;
};

// Bodies an entity's own casts must ignore: its hitboxes, the vehicle it sits
// in. Without this a shooter's first ray hits its own chest.
constexpr u32 kMaxIgnoredBodies = 8;
struct HitIgnoreList {
  u64 bodies[kMaxIgnoredBodies] = {};  // physics::BodyId; 0 entries are skipped
  u8 count = 0;
};

// --- weapons --------------------------------------------------------------

// The mutable half of a weapon: what the definition looks like right now.
struct WeaponState {
  WeaponDefId def = kInvalidWeaponDef;
  u32 ammo = 0;     // rounds in the magazine
  u32 reserve = 0;  // rounds carried for it
  f32 cooldown = 0;      // s until the next round may leave
  f32 reload_timer = 0;  // s left of the reload in progress; 0 = not reloading
  bool reload_empty = false;   // the reload in progress started from empty
  bool reload_shells = false;  // per-round reload in progress
  u32 burst_remaining = 0;     // rounds left in the burst being fired
  f32 bloom = 0;               // [0..1] accumulated spread
  f32 ads = 0;                 // [0..1] aim-down-sights blend
  bool trigger_latched = false;  // the trigger has not been released since firing
  u32 shots_fired = 0;           // lifetime rounds, for stats and pattern indexing
  u32 rng = 0x9E3779B9u;         // deterministic spread / recoil jitter stream
};

// Up to four weapons on one entity, one of them raised. Fixed size keeps the
// component POD; a game that wants a bigger arsenal keeps the rest in its own
// storage and swaps definitions into a slot.
constexpr u8 kMaxWeaponSlots = 4;
struct Loadout {
  WeaponState slots[kMaxWeaponSlots];
  u8 count = 0;    // populated slots
  u8 active = 0;   // slot currently raised
  u8 pending = 0;  // slot being switched to while swap_timer runs
  f32 swap_timer = 0;
};

// Written by the game every fixed step, edge fields consumed by StepWeapons.
struct WeaponIntent {
  bool trigger = false;  // held state, not an edge: semi-auto latches it itself
  bool aim = false;      // aim-down-sights held
  bool reload = false;   // rising edge this step
  i8 switch_to = -1;     // slot to raise, -1 for no request; consumed

  Vec3 origin = {0, 0, 0};       // where rounds leave from (the eye, usually)
  Vec3 direction = {0, 0, -1};   // normalized aim direction

  // The shooter's kinematics this step. Spread and the viewmodel read them; the
  // game copies them out of character::CharacterState (or whatever moves it).
  f32 speed = 0;         // m/s, horizontal
  bool airborne = false;
  bool crouched = false;
  f32 look_yaw_delta = 0;    // rad applied to the view this step (viewmodel sway)
  f32 look_pitch_delta = 0;
};

// View kick. StepViewRecoil turns it into `view_*_delta`, which the game adds
// to its own look deltas for the step. Recoil therefore composes with player
// look input instead of fighting the camera rig for ownership of the view
// angles, and it works the same whether the view yaw lives on a character
// heading or on a free camera.
//
// A shot pushes its kick into `pending`, which drains into the view over
// `kick_half_life` (a punch, not a teleport). `recovery_fraction` of what was
// pushed is remembered as `recoverable` and pulled back out of the view after
// `recovery_delay`, so a burst climbs and then settles most of the way back
// down on its own; the rest is the player's to compensate.
struct ViewRecoil {
  // Written by StepViewRecoil: the view rotation recoil asks for THIS step,
  // kick minus recovery, in radians. Positive pitch is up. The game adds them
  // to the look deltas it fills in (character::CharacterIntent for a player,
  // scene::CameraIntent for a free camera).
  f32 view_pitch_delta = 0;
  f32 view_yaw_delta = 0;

  f32 pending_pitch = 0;
  f32 pending_yaw = 0;
  f32 recoverable_pitch = 0;
  f32 recoverable_yaw = 0;
  f32 time_since_shot = 0;

  f32 kick_half_life = 0.03f;
  f32 recovery_half_life = 0.16f;
  f32 recovery_delay = 0.1f;
  f32 recovery_fraction = 0.75f;  // [0..1] of the kick that returns by itself
};

// First-person weapon pose, in view space (+x right, +y up, +z forward). The
// renderer is free to ignore it; a game that draws a viewmodel mesh multiplies
// the camera matrix by this offset. Inputs come from the WeaponIntent beside it.
struct Viewmodel {
  // Tuning.
  f32 sway_scale = 0.012f;      // metres of counter-offset per rad/s of look rate
  f32 sway_max = 0.05f;         // metres, hard cap
  f32 sway_half_life = 0.09f;
  f32 bob_amplitude = 0.022f;   // metres at the reference speed
  f32 bob_rate = 9.0f;          // rad/s of bob cycle at the reference speed
  f32 bob_reference_speed = 4.2f;  // m/s that maps to full bob
  f32 bob_half_life = 0.12f;
  Vec3 ads_offset = {-0.06f, -0.01f, 0.07f};  // hip -> aimed pose
  f32 punch_scale = 0.03f;      // metres pushed back per shot
  f32 punch_half_life = 0.07f;
  f32 roll_scale = 0.6f;        // rad of roll per metre of lateral sway

  // State.
  Vec3 offset = {0, 0, 0};
  f32 pitch = 0;
  f32 yaw = 0;
  f32 roll = 0;
  f32 bob_phase = 0;
  f32 bob_weight = 0;
  f32 punch = 0;
};

// --- projectiles ----------------------------------------------------------

// A round in flight. Spawned by StepWeapons for kProjectile weapons (and by
// SpawnProjectile for anything a game throws), advanced by StepProjectiles,
// which sweeps the segment it covered this step against the physics world.
struct Projectile {
  ecs::Entity owner{};      // credited with the damage
  WeaponDefId def = kInvalidWeaponDef;
  Vec3 position = {0, 0, 0};
  Vec3 velocity = {0, 0, 0};
  f32 damage = 0;
  f32 impulse = 0;
  f32 gravity = 9.81f;
  f32 drag = 0;             // 1/m, fraction of speed lost per metre travelled
  f32 radius = 0;           // >0 sweeps a sphere
  f32 age = 0;
  f32 life = 8.0f;
  f32 blast_radius = 0;
  f32 blast_damage = 0;
  f32 blast_min_scale = 0.15f;
  f32 blast_impulse = 0;
  bool explode_on_expire = false;
  u64 ignore[4] = {};  // owner bodies, copied from its HitIgnoreList at spawn
  u8 ignore_count = 0;
};

}  // namespace rx::combat

#endif  // RX_COMBAT_COMPONENTS_H_
