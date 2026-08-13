#include "anim/body_dynamics.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "asset/asset_id.h"

namespace rx::anim {
namespace {

constexpr f32 kPi = 3.14159265358979323846f;
constexpr f32 kAccelerationFilterHalflife = 0.025f;

Vec3 Multiply(const Vec3 &a, const Vec3 &b) {
  return {a.x * b.x, a.y * b.y, a.z * b.z};
}

Vec3 ClampMagnitude(Vec3 value, f32 maximum) {
  const f32 length = Length(value);
  return maximum > 0 && length > maximum ? value * (maximum / length) : value;
}

void ClampAxes(Vec3 *value, Vec3 *velocity, const Vec3 &limit) {
  auto clamp = [](f32 *x, f32 *v, f32 extent) {
    extent = std::max(extent, 0.0f);
    const f32 before = *x;
    *x = std::clamp(*x, -extent, extent);
    if (*x != before && ((*x > 0 && *v > 0) || (*x < 0 && *v < 0)))
      *v = 0;
  };
  clamp(&value->x, &velocity->x, limit.x);
  clamp(&value->y, &velocity->y, limit.y);
  clamp(&value->z, &velocity->z, limit.z);
}

// Exact solution of x'' + 2*zeta*w*x' + w^2*x = force for a constant force
// over dt. This remains stable across hitches and supports under-, critical-,
// and over-damped authored material responses.
void Spring(f32 *x, f32 *velocity, f32 force, f32 frequency, f32 damping,
            f32 dt) {
  const f32 w = 2.0f * kPi * std::max(frequency, 0.01f);
  const f32 target = force / (w * w);
  const f32 y = *x - target;
  const f32 v = *velocity;
  const f32 z = std::max(damping, 0.0f);

  if (z < 1.0f - 1e-4f) {
    const f32 wd = w * std::sqrt(1.0f - z * z);
    const f32 e = std::exp(-z * w * dt);
    const f32 c = std::cos(wd * dt);
    const f32 s = std::sin(wd * dt);
    const f32 a = (v + z * w * y) / wd;
    *x = target + e * (y * c + a * s);
    *velocity = e * (v * c - ((z * w * v + w * w * y) / wd) * s);
  } else if (z <= 1.0f + 1e-4f) {
    const f32 e = std::exp(-w * dt);
    const f32 a = v + w * y;
    *x = target + (y + a * dt) * e;
    *velocity = (v - w * a * dt) * e;
  } else {
    const f32 root = std::sqrt(z * z - 1.0f);
    const f32 r1 = -w * (z - root);
    const f32 r2 = -w * (z + root);
    const f32 c1 = (v - r2 * y) / (r1 - r2);
    const f32 c2 = y - c1;
    const f32 e1 = std::exp(r1 * dt);
    const f32 e2 = std::exp(r2 * dt);
    *x = target + c1 * e1 + c2 * e2;
    *velocity = r1 * c1 * e1 + r2 * c2 * e2;
  }
}

void Spring(Vec3 *x, Vec3 *velocity, const Vec3 &force, f32 frequency,
            f32 damping, f32 dt) {
  Spring(&x->x, &velocity->x, force.x, frequency, damping, dt);
  Spring(&x->y, &velocity->y, force.y, frequency, damping, dt);
  Spring(&x->z, &velocity->z, force.z, frequency, damping, dt);
}

Vec3 ExpSmooth(const Vec3 &previous, const Vec3 &value, f32 halflife, f32 dt) {
  const f32 t = 1.0f - std::exp(-0.69314718f * dt / std::max(halflife, 1e-4f));
  return Lerp(previous, value, t);
}

Vec3 QuaternionVelocity(Quat previous, Quat current, f32 dt) {
  // current * inverse(previous) expresses the shortest delta in model axes;
  // callers can then transform it into each driver's current local frame.
  Quat delta = Normalize(current * Conjugate(previous));
  if (delta.w < 0)
    delta = {-delta.x, -delta.y, -delta.z, -delta.w};
  const f32 sin_half =
      std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
  if (sin_half < 1e-6f || dt <= 0)
    return {};
  const f32 angle =
      2.0f * std::atan2(sin_half, std::clamp(delta.w, -1.0f, 1.0f));
  return Vec3{delta.x, delta.y, delta.z} * (angle / (sin_half * dt));
}

Quat Euler(const Vec3 &radians) {
  return QuatFromAxisAngle({1, 0, 0}, radians.x) *
         QuatFromAxisAngle({0, 1, 0}, radians.y) *
         QuatFromAxisAngle({0, 0, 1}, radians.z);
}

f32 Signal(const BodyRegionSample &sample, BodyDeformationSignal signal) {
  switch (signal) {
  case BodyDeformationSignal::kStretch:
    return sample.stretch;
  case BodyDeformationSignal::kCompression:
    return sample.compression;
  case BodyDeformationSignal::kShear:
    return sample.shear;
  case BodyDeformationSignal::kSpeed:
    return sample.speed;
  case BodyDeformationSignal::kImpact:
    return sample.impact;
  }
  return 0;
}

void AppendMorphSignals(const BodyRegionConfig &config,
                        const BodyRegionSample &sample,
                        base::Vector<BodyMorphWeight> *morphs) {
  if (!morphs)
    return;
  for (const BodyMorphBinding &binding : config.morphs) {
    BodyMorphWeight weight;
    weight.target = asset::MakeAssetId(binding.target).hash;
    weight.weight =
        std::clamp(binding.bias + binding.gain * Signal(sample, binding.signal),
                   binding.min_weight, binding.max_weight);
    if (weight.target == 0 || weight.weight == 0)
      continue;
    bool combined = false;
    for (BodyMorphWeight &existing : *morphs) {
      if (existing.target != weight.target)
        continue;
      existing.weight += weight.weight;
      combined = true;
      break;
    }
    if (!combined)
      morphs->push_back(weight);
  }
}

} // namespace

BodyRegionConfig MakeBodyRegionPreset(BodyRegionKind kind,
                                      std::string_view name,
                                      std::string_view driver_bone,
                                      std::string_view driven_bone) {
  BodyRegionConfig out;
  out.name = std::string(name);
  out.driver_bone = std::string(driver_bone);
  out.driven_bone = std::string(driven_bone);
  switch (kind) {
  case BodyRegionKind::kChest:
    out.frequency_hz = 3.1f;
    out.damping_ratio = 0.58f;
    out.translation_mobility = {0.55f, 0.9f, 1.0f};
    out.max_translation = {0.025f, 0.045f, 0.055f};
    out.rotation_mobility = {0.65f, 0.25f, 0.45f};
    out.max_rotation = {0.18f, 0.07f, 0.13f};
    out.deformation_axis = {0, 1, 0};
    break;
  case BodyRegionKind::kAbdomen:
    out.frequency_hz = 2.7f;
    out.damping_ratio = 0.72f;
    out.translation_mobility = {0.45f, 0.75f, 0.9f};
    out.max_translation = {0.025f, 0.04f, 0.05f};
    out.rotation_mobility = {0.35f, 0.2f, 0.35f};
    out.max_rotation = {0.10f, 0.06f, 0.10f};
    out.deformation_axis = {0, 1, 0};
    break;
  case BodyRegionKind::kGlutes:
    out.frequency_hz = 3.4f;
    out.damping_ratio = 0.64f;
    out.translation_mobility = {0.55f, 0.75f, 1.0f};
    out.max_translation = {0.025f, 0.04f, 0.055f};
    out.rotation_mobility = {0.55f, 0.2f, 0.35f};
    out.max_rotation = {0.14f, 0.06f, 0.10f};
    out.deformation_axis = {0, 1, 0};
    break;
  case BodyRegionKind::kThigh:
    out.frequency_hz = 4.1f;
    out.damping_ratio = 0.74f;
    out.translation_mobility = {0.45f, 0.35f, 0.6f};
    out.max_translation = {0.02f, 0.025f, 0.03f};
    out.rotation_mobility = {0.3f, 0.18f, 0.3f};
    out.max_rotation = {0.08f, 0.05f, 0.08f};
    break;
  case BodyRegionKind::kUpperArm:
    out.frequency_hz = 4.6f;
    out.damping_ratio = 0.78f;
    out.translation_mobility = {0.35f, 0.3f, 0.45f};
    out.max_translation = {0.015f, 0.02f, 0.025f};
    out.rotation_mobility = {0.25f, 0.15f, 0.25f};
    out.max_rotation = {0.06f, 0.04f, 0.06f};
    break;
  case BodyRegionKind::kCalf:
    out.frequency_hz = 5.2f;
    out.damping_ratio = 0.82f;
    out.translation_mobility = {0.25f, 0.2f, 0.35f};
    out.max_translation = {0.012f, 0.015f, 0.02f};
    out.rotation_mobility = {0.18f, 0.1f, 0.18f};
    out.max_rotation = {0.045f, 0.03f, 0.045f};
    break;
  case BodyRegionKind::kCustom:
    break;
  }
  return out;
}

u32 BodyDynamics::AddRegion(const BodyRegionConfig &config) {
  regions_.push_back(config);
  states_.push_back({});
  resolved_ = false;
  return static_cast<u32>(regions_.size() - 1);
}

bool BodyDynamics::SetRegion(u32 index, const BodyRegionConfig &config) {
  if (index >= regions_.size())
    return false;
  regions_[index] = config;
  states_[index] = {};
  resolved_ = false;
  return true;
}

const BodyRegionConfig *BodyDynamics::region(u32 index) const {
  return index < regions_.size() ? &regions_[index] : nullptr;
}

void BodyDynamics::ClearRegions() {
  regions_.clear();
  states_.clear();
  model_.clear();
  skeleton_hash_ = 0;
  skeleton_bones_ = 0;
  resolved_ = false;
}

void BodyDynamics::Reset() {
  for (State &state : states_) {
    const i32 driver = state.driver;
    const i32 driven = state.driven;
    const bool active = state.active;
    state = {};
    state.driver = driver;
    state.driven = driven;
    state.active = active;
  }
}

void BodyDynamics::Resolve(const asset::Skeleton &skeleton) {
  states_.resize(regions_.size());
  for (u32 i = 0; i < regions_.size(); ++i) {
    states_[i] = {};
    states_[i].driver = skeleton.Find(regions_[i].driver_bone);
    states_[i].driven = skeleton.Find(regions_[i].driven_bone);
    states_[i].active =
        regions_[i].enabled && states_[i].driver >= 0 && states_[i].driven >= 0;
  }
  skeleton_hash_ = skeleton.id.hash;
  skeleton_bones_ = static_cast<u32>(skeleton.bones.size());
  resolved_ = true;
}

void BodyDynamics::Update(const asset::Skeleton &skeleton,
                          const BodyDynamicsFrame &frame, f32 dt,
                          SkeletonPose *pose,
                          base::Vector<BodyMorphWeight> *morphs) {
  if (morphs)
    morphs->clear();
  if (!pose || pose->size() != skeleton.bones.size())
    return;
  if (!resolved_ || skeleton_hash_ != skeleton.id.hash ||
      skeleton_bones_ != skeleton.bones.size() ||
      states_.size() != regions_.size()) {
    Resolve(skeleton);
  }
  const f32 model_units_per_metre = std::max(
      std::isfinite(frame.model_units_per_metre) ? frame.model_units_per_metre
                                                 : 1.0f,
      1e-6f);
  if (std::fabs(model_units_per_metre - model_units_per_metre_) > 1e-5f) {
    Reset();
    model_units_per_metre_ = model_units_per_metre;
  }
  if (frame.teleport || !std::isfinite(dt) || dt <= 0 || dt > 0.25f)
    Reset();

  ComputeModelMatrices(skeleton, *pose, &model_);
  if (dt <= 0 || dt > 0.25f)
    dt = 0;

  for (u32 i = 0; i < regions_.size(); ++i) {
    const BodyRegionConfig &config = regions_[i];
    State &state = states_[i];
    if (!state.active)
      continue;

    const Vec3 driver_position =
        Translation(model_[state.driver]) * (1.0f / model_units_per_metre);
    const Quat driver_orientation = QuatFromMat4(model_[state.driver]);
    const bool discontinuity =
        state.initialized && teleport_distance_ > 0 &&
        Length(driver_position - state.previous_position) > teleport_distance_;
    if (!state.initialized || discontinuity || frame.teleport || dt <= 0) {
      state.translation = {};
      state.velocity = {};
      state.rotation = {};
      state.angular_velocity = {};
      state.previous_driver_velocity = {};
      state.previous_driver_angular_velocity = {};
      state.filtered_acceleration = {};
      state.filtered_angular_acceleration = {};
      state.impact = 0;
      state.previous_position = driver_position;
      state.previous_orientation = driver_orientation;
      state.initialized = true;
      AppendMorphSignals(config, sample(i), morphs);
      continue;
    }

    const Vec3 driver_velocity =
        (driver_position - state.previous_position) * (1.0f / dt);
    const Vec3 driver_angular_velocity =
        QuaternionVelocity(state.previous_orientation, driver_orientation, dt);
    Vec3 inferred_acceleration =
        (driver_velocity - state.previous_driver_velocity) * (1.0f / dt);
    Vec3 inferred_angular_acceleration =
        (driver_angular_velocity - state.previous_driver_angular_velocity) *
        (1.0f / dt);
    inferred_acceleration =
        ClampMagnitude(inferred_acceleration, max_acceleration_);
    inferred_angular_acceleration =
        ClampMagnitude(inferred_angular_acceleration, max_acceleration_);
    state.filtered_acceleration =
        ExpSmooth(state.filtered_acceleration, inferred_acceleration,
                  kAccelerationFilterHalflife, dt);
    state.filtered_angular_acceleration = ExpSmooth(
        state.filtered_angular_acceleration, inferred_angular_acceleration,
        kAccelerationFilterHalflife, dt);

    const Quat inverse_driver = Conjugate(driver_orientation);
    const Vec3 acceleration_local =
        Rotate(inverse_driver, ClampMagnitude(state.filtered_acceleration +
                                                  frame.linear_acceleration,
                                              max_acceleration_));
    const Vec3 gravity_local = Rotate(inverse_driver, frame.gravity);
    const Vec3 angular_acceleration_local = Rotate(
        inverse_driver, ClampMagnitude(state.filtered_angular_acceleration +
                                           frame.angular_acceleration,
                                       max_acceleration_));

    state.velocity += Multiply(Rotate(inverse_driver, frame.linear_impulse),
                               config.translation_mobility) *
                      config.inertia_scale;
    state.angular_velocity +=
        Multiply(Rotate(inverse_driver, frame.angular_impulse),
                 config.rotation_mobility) *
        config.angular_inertia_scale;

    const Vec3 linear_force =
        Multiply(gravity_local * config.gravity_scale -
                     acceleration_local * config.inertia_scale,
                 config.translation_mobility);
    const Vec3 angular_force =
        Multiply(angular_acceleration_local * -config.angular_inertia_scale,
                 config.rotation_mobility);
    Spring(&state.translation, &state.velocity, linear_force,
           config.frequency_hz, config.damping_ratio, dt);
    Spring(&state.rotation, &state.angular_velocity, angular_force,
           config.frequency_hz, config.damping_ratio, dt);
    ClampAxes(&state.translation, &state.velocity, config.max_translation);
    ClampAxes(&state.rotation, &state.angular_velocity, config.max_rotation);

    const f32 impact_from_acceleration =
        std::max(Length(acceleration_local) - config.impact_threshold, 0.0f) /
        std::max(max_acceleration_ - config.impact_threshold, 1.0f);
    const f32 impact_from_impulse = Length(frame.linear_impulse) / 2.0f;
    state.impact = std::max(
        state.impact *
            std::exp(-0.69314718f * dt / std::max(config.impact_decay, 1e-4f)),
        std::clamp(std::max(impact_from_acceleration, impact_from_impulse),
                   0.0f, 1.0f));

    // Translate from the driver's local axes into model space, then into the
    // driven bone parent's local frame. This remains correct when driver and
    // driven bones have different parents.
    const Vec3 driver_local_translation =
        Multiply(state.translation, config.translation_gain);
    const Vec3 model_translation =
        Rotate(driver_orientation, driver_local_translation) *
        model_units_per_metre;
    const i32 parent = skeleton.bones[state.driven].parent;
    const Quat parent_orientation =
        parent >= 0 ? QuatFromMat4(model_[parent]) : Quat{0, 0, 0, 1};
    pose->translation[state.driven] +=
        Rotate(Conjugate(parent_orientation), model_translation);
    const Quat driven_orientation = QuatFromMat4(model_[state.driven]);
    const Vec3 driver_rotation = Multiply(state.rotation, config.rotation_gain);
    const Vec3 model_rotation = Rotate(driver_orientation, driver_rotation);
    const Vec3 driven_local_rotation =
        Rotate(Conjugate(driven_orientation), model_rotation);
    pose->rotation[state.driven] =
        Normalize(pose->rotation[state.driven] * Euler(driven_local_rotation));

    AppendMorphSignals(config, sample(i), morphs);

    state.previous_position = driver_position;
    state.previous_orientation = driver_orientation;
    state.previous_driver_velocity = driver_velocity;
    state.previous_driver_angular_velocity = driver_angular_velocity;
  }
}

BodyRegionSample BodyDynamics::sample(u32 index) const {
  BodyRegionSample out;
  if (index >= states_.size() || index >= regions_.size())
    return out;
  const State &state = states_[index];
  const BodyRegionConfig &config = regions_[index];
  out.translation = state.translation;
  out.velocity = state.velocity;
  out.rotation = state.rotation;
  out.impact = state.impact;
  out.active = state.active && state.initialized;

  const Vec3 axis = Normalize(config.deformation_axis);
  const f32 axial = Dot(state.translation, axis);
  const f32 normalizer =
      std::max(Length(Multiply(axis, config.max_translation)), 1e-5f);
  out.stretch = std::clamp(axial / normalizer, 0.0f, 1.0f);
  out.compression = std::clamp(-axial / normalizer, 0.0f, 1.0f);
  const Vec3 lateral = state.translation - axis * axial;
  out.shear = std::clamp(Length(lateral) /
                             std::max(Length(config.max_translation -
                                             Multiply(Multiply(axis, axis),
                                                      config.max_translation)),
                                      1e-5f),
                         0.0f, 1.0f);
  out.speed = std::clamp(
      Length(state.velocity) /
          std::max(Length(config.max_translation) * config.frequency_hz, 1e-5f),
      0.0f, 1.0f);
  return out;
}

void ApplyBodyMorphWeights(const asset::Mesh &mesh,
                           const base::Vector<BodyMorphWeight> &body,
                           base::Vector<f32> *dense, f32 min_weight,
                           f32 max_weight) {
  if (!dense)
    return;
  dense->resize(mesh.morph_targets.size());
  for (const BodyMorphWeight &body_weight : body) {
    const i32 target = mesh.FindMorphTarget(body_weight.target);
    if (target < 0)
      continue;
    (*dense)[target] = std::clamp((*dense)[target] + body_weight.weight,
                                  min_weight, max_weight);
  }
}

} // namespace rx::anim
