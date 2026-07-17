// StateEstimator + ContactEstimator: measure the simulated body every fixed
// step (docs/LOCOMOTION.md). No assumptions from the previous plan; every read
// goes through the physics adapter.

#include "locomotion/estimator.h"

#include <cmath>

#include "locomotion/internal_math.h"

namespace rx::locomotion {
using namespace internal;

void StateEstimator::Measure(const physics::PhysicsWorld& physics, const BipedRig& rig,
                             const PhysicalModifiers& modifiers, CharacterMeasurements* out) const {
  *out = CharacterMeasurements{};
  bool ok = true;

  // Root pose/velocity from the pelvis body.
  const physics::BodyId pelvis = rig.body[static_cast<u32>(BodyPart::kPelvis)];
  Vec3 root_pos;
  f32 root_rot[4] = {0, 0, 0, 1};
  ok = physics.GetBodyTransform(pelvis, &root_pos, root_rot) && ok;
  ok = physics.GetBodyVelocity(pelvis, &out->root_linear_velocity, &out->root_angular_velocity) &&
       ok;
  out->root_position = root_pos;
  out->root_rotation = {root_rot[0], root_rot[1], root_rot[2], root_rot[3]};

  // Mass-weighted COM and COM velocity over all 13 bodies, plus carried mass.
  Vec3 com_sum{};
  Vec3 vel_sum{};
  f32 mass_sum = 0;
  for (u32 i = 0; i < kBodyPartCount; ++i) {
    const physics::BodyId id = rig.body[i];
    // Masses were resolved at build (rig.body_mass[]); no need to take a Jolt
    // body lock per body per tick. A failed COM/velocity read below still marks
    // the measurement invalid, so a dead body cannot slip through.
    const f32 m = rig.body_mass[i];
    Vec3 com;
    Vec3 lin;
    if (m <= 0 || !physics.GetBodyCenterOfMass(id, &com) ||
        !physics.GetBodyVelocity(id, &lin, nullptr)) {
      ok = false;
      continue;
    }
    com_sum += com * m;
    vel_sum += lin * m;
    mass_sum += m;
  }
  // Carried mass rides with the pelvis (position in pelvis space, velocity = the
  // pelvis linear velocity).
  if (modifiers.carried_mass > 0) {
    const Vec3 carried_world =
        root_pos + Rotate(out->root_rotation, modifiers.carried_mass_local_offset);
    com_sum += carried_world * modifiers.carried_mass;
    vel_sum += out->root_linear_velocity * modifiers.carried_mass;
    mass_sum += modifiers.carried_mass;
  }
  if (mass_sum > 0) {
    out->com_position = com_sum * (1.0f / mass_sum);
    out->com_velocity = vel_sum * (1.0f / mass_sum);
  } else {
    ok = false;
  }

  // Per-foot measurement.
  f32 min_sole_y = 0;
  bool have_sole = false;
  Vec3 normal_accum{};
  u32 ground_feet = 0;
  for (u32 f = 0; f < kFootCount; ++f) {
    FootMeasurement& fm = out->foot[f];
    const physics::BodyId foot_id =
        rig.body[static_cast<u32>(f == 0 ? BodyPart::kFootL : BodyPart::kFootR)];
    fm.position = rig.SolePosition(physics, f);
    Vec3 foot_vel;
    if (physics.GetBodyVelocity(foot_id, &foot_vel, nullptr)) {
      fm.velocity = foot_vel;
    } else {
      ok = false;
    }

    physics::PhysicsWorld::BodyContact contacts[kBodyPartCount + 4];
    const u32 n = physics.GetBodyContacts(foot_id, contacts, kBodyPartCount + 4);
    Vec3 normal_sum{};
    f32 impulse_sum = 0;
    bool up_contact = false;
    for (u32 c = 0; c < n; ++c) {
      if (rig.ContainsBody(contacts[c].other)) continue;
      if (contacts[c].normal.y > 0.5f) {
        up_contact = true;
        const f32 w = contacts[c].impulse > 0 ? contacts[c].impulse : 1.0f;
        normal_sum += contacts[c].normal * w;
        impulse_sum += contacts[c].impulse;
      }
    }
    fm.in_contact = up_contact;
    if (up_contact) {
      fm.contact_normal = Length(normal_sum) > 0 ? Normalize(normal_sum) : Vec3{0, 1, 0};
      fm.contact_impulse = impulse_sum;
      fm.slip_speed = PlanarLength(fm.velocity);
      normal_accum += fm.contact_normal;
      ++ground_feet;
    } else {
      fm.contact_normal = {0, 1, 0};
      fm.contact_impulse = 0;
      fm.slip_speed = 0;
    }

    if (!have_sole || fm.position.y < min_sole_y) {
      min_sole_y = fm.position.y;
      have_sole = true;
    }
  }

  out->ground_normal = ground_feet > 0 && Length(normal_accum) > 0 ? Normalize(normal_accum)
                                                                   : Vec3{0, 1, 0};

  // Crown height: the head sphere's radius comes straight from the rig geometry
  // (rig.head_radius), so crown = head COM y + head_radius.
  Vec3 head_com;
  if (physics.GetBodyCenterOfMass(rig.body[static_cast<u32>(BodyPart::kHead)], &head_com)) {
    out->estimated_body_height = (head_com.y + rig.head_radius) - min_sole_y;
  } else {
    ok = false;
  }

  // Finite sweep over everything stored.
  bool finite = FiniteV(out->root_position) && FiniteQ(out->root_rotation) &&
                FiniteV(out->root_linear_velocity) && FiniteV(out->root_angular_velocity) &&
                FiniteV(out->com_position) && FiniteV(out->com_velocity) &&
                FiniteV(out->ground_normal) && std::isfinite(out->estimated_body_height);
  for (u32 f = 0; f < kFootCount; ++f) {
    const FootMeasurement& fm = out->foot[f];
    finite = finite && FiniteV(fm.position) && FiniteV(fm.velocity) && FiniteV(fm.contact_normal) &&
             std::isfinite(fm.contact_impulse) && std::isfinite(fm.slip_speed);
  }

  if (ok && finite) {
    out->gravity = Length(physics.gravity());  // measured |world gravity|, m/s^2
    out->valid = true;
  } else {
    *out = CharacterMeasurements{};
    out->valid = false;
  }
}

void ContactEstimator::Reset() {
  estimate_ = ContactEstimate{};
  for (u32 f = 0; f < kFootCount; ++f) {
    pending_phase_[f] = FootPhase::kUnconfirmed;
    pending_ticks_[f] = 0;
  }
}

void ContactEstimator::Update(const CharacterMeasurements& m, f32 dt) {
  Vec3 support_sum{};
  u32 support_positions = 0;  // supporting + sliding, for the support centre
  u32 support_count = 0;      // supporting only

  for (u32 f = 0; f < kFootCount; ++f) {
    const FootMeasurement& fm = m.foot[f];

    // Raw classification for this tick.
    FootPhase raw;
    if (!fm.in_contact) {
      raw = FootPhase::kSwinging;
    } else if (fm.slip_speed >= 0.6f || fm.contact_normal.y <= 0.6f ||
               std::abs(fm.velocity.y) >= 0.35f) {
      raw = FootPhase::kSliding;
    } else {
      raw = FootPhase::kSupporting;
    }

    FootPhase& current = estimate_.phase[f];
    if (raw == current) {
      pending_ticks_[f] = 0;
      pending_phase_[f] = current;
    } else {
      if (raw != pending_phase_[f]) {
        pending_phase_[f] = raw;
        pending_ticks_[f] = 1;
      } else {
        ++pending_ticks_[f];
      }
      // Dwell required before the change commits.
      u32 required = 2;
      if (current != FootPhase::kSupporting && raw == FootPhase::kSupporting) {
        required = 2;  // enter supporting
      } else if (current == FootPhase::kSupporting && raw != FootPhase::kSupporting) {
        required = 3;  // leave supporting
      }
      if (pending_ticks_[f] >= required) {
        current = raw;
        pending_ticks_[f] = 0;
        estimate_.phase_time[f] = 0;
      }
    }

    estimate_.phase_time[f] += dt;

    if (current == FootPhase::kSupporting) {
      ++support_count;
      support_sum += fm.position;
      ++support_positions;
    } else if (current == FootPhase::kSliding) {
      support_sum += fm.position;
      ++support_positions;
    }
  }

  estimate_.support_count = support_count;
  if (support_positions > 0) {
    estimate_.support_center = support_sum * (1.0f / static_cast<f32>(support_positions));
  }
  // else keep the previous support_center.
}

}  // namespace rx::locomotion
