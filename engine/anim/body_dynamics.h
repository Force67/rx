#ifndef RX_ANIM_BODY_DYNAMICS_H_
#define RX_ANIM_BODY_DYNAMICS_H_

#include <string>
#include <string_view>

#include <base/containers/vector.h>

#include "anim/pose.h"
#include "asset/mesh.h"
#include "asset/skeleton.h"
#include "core/export.h"
#include "core/math.h"
#include "core/types.h"

namespace rx::anim {

// Signals produced by a soft-tissue region. They can drive any authored morph
// target, so shape change remains an asset concern while motion stays generic.
enum class BodyDeformationSignal : u8 {
  kStretch,
  kCompression,
  kShear,
  kSpeed,
  kImpact,
};

struct BodyMorphBinding {
  std::string target;
  BodyDeformationSignal signal = BodyDeformationSignal::kStretch;
  f32 gain = 1;
  f32 bias = 0;
  f32 min_weight = 0;
  f32 max_weight = 1;
};

// A separately authored soft region. `driver_bone` supplies the animated
// reference frame and `driven_bone` receives the secondary translation and
// rotation. Usually the driven bone is a dedicated tissue/helper bone; it can
// equal the driver for simpler rigs. All vectors use the driver's local frame.
struct BodyRegionConfig {
  std::string name;
  std::string driver_bone;
  std::string driven_bone;

  // Physical response. Frequency controls firmness and damping_ratio controls
  // decay (1 = critical, <1 = a decaying jiggle). These values describe a
  // second-order spring and are integrated analytically, independent of frame
  // rate. Gravity creates natural sag; acceleration creates inertial lag.
  f32 frequency_hz = 3.5f;
  f32 damping_ratio = 0.65f;
  f32 gravity_scale = 1;
  f32 inertia_scale = 1;
  f32 angular_inertia_scale = 0.35f;

  // Per-axis compliance masks. Zero locks an axis. Translation is metres;
  // rotation is radians. Limits are symmetric and prevent implausible poses
  // during teleports, hitches, or extreme gameplay impulses.
  Vec3 translation_mobility = {0.65f, 1.0f, 0.8f};
  Vec3 max_translation = {0.035f, 0.055f, 0.045f};
  Vec3 rotation_mobility = {0.5f, 0.35f, 0.5f};
  Vec3 max_rotation = {0.16f, 0.10f, 0.16f};
  Vec3 translation_gain = {1, 1, 1};
  Vec3 rotation_gain = {1, 1, 1};

  // Axis used to separate stretch/compression from lateral shear. It need not
  // be normalized. The normalized signals reach one at the corresponding
  // configured positional limit.
  Vec3 deformation_axis = {0, 1, 0};
  f32 impact_threshold = 8.0f; // m/s^2 before impact deformation begins
  f32 impact_decay = 0.14f;    // seconds

  base::Vector<BodyMorphBinding> morphs;
  bool enabled = true;
};

// Standard tuning is deliberately anatomy-aware but rig-name agnostic. Games
// choose the appropriate kind for every authored helper bone and can freely
// adjust or add regions. Rigid joints and the head are intentionally absent.
enum class BodyRegionKind : u8 {
  kChest,
  kAbdomen,
  kGlutes,
  kThigh,
  kUpperArm,
  kCalf,
  kCustom,
};

RX_ANIM_EXPORT BodyRegionConfig MakeBodyRegionPreset(
    BodyRegionKind kind, std::string_view name, std::string_view driver_bone,
    std::string_view driven_bone);

// Motion not visible in the skeleton pose (for example, entity locomotion)
// must be supplied here in skeleton/model axes. Animation-induced acceleration
// of each driver bone is measured automatically. Impulses are velocity changes
// and should be staged only on the frame where they occur.
struct BodyDynamicsFrame {
  Vec3 linear_acceleration = {0, 0, 0};  // m/s^2
  Vec3 angular_acceleration = {0, 0, 0}; // rad/s^2
  Vec3 linear_impulse = {0, 0, 0};       // m/s
  Vec3 angular_impulse = {0, 0, 0};      // rad/s
  Vec3 gravity = {0, -9.81f, 0};         // m/s^2; zero in free fall
  // Skeleton/model units corresponding to one metre. Source-native rigs can
  // use values such as 70 while metre-authored glTF rigs keep the default.
  f32 model_units_per_metre = 1;
  bool teleport = false;
};

struct BodyMorphWeight {
  u64 target = 0; // asset::MakeAssetId(source morph name).hash
  f32 weight = 0;
};

struct BodyRegionSample {
  Vec3 translation;
  Vec3 velocity;
  Vec3 rotation;
  f32 stretch = 0;
  f32 compression = 0;
  f32 shear = 0;
  f32 speed = 0;
  f32 impact = 0;
  bool active = false;
};

// Stateful, per-character secondary-motion layer. Call Update after the base
// clip/graph and IK have written `pose`, but before ComputeModelMatrices builds
// the skin palette. One controller must not be shared by multiple characters.
class RX_ANIM_EXPORT BodyDynamics {
public:
  u32 AddRegion(const BodyRegionConfig &config);
  bool SetRegion(u32 index, const BodyRegionConfig &config);
  const BodyRegionConfig *region(u32 index) const;
  u32 region_count() const { return static_cast<u32>(regions_.size()); }
  void ClearRegions();

  // Clears history. The next Update initializes at rest without a one-frame
  // kick. Update also resets automatically for teleports, invalid dt, skeleton
  // changes, and driver movement larger than teleport_distance.
  void Reset();
  void set_teleport_distance(f32 metres) { teleport_distance_ = metres; }
  void set_max_acceleration(f32 metres_per_second2) {
    max_acceleration_ = metres_per_second2;
  }

  void Update(const asset::Skeleton &skeleton, const BodyDynamicsFrame &frame,
              f32 dt, SkeletonPose *pose,
              base::Vector<BodyMorphWeight> *morphs = nullptr);

  BodyRegionSample sample(u32 index) const;

private:
  struct State {
    i32 driver = -1;
    i32 driven = -1;
    Vec3 translation;
    Vec3 velocity;
    Vec3 rotation;
    Vec3 angular_velocity;
    Vec3 previous_position;
    Quat previous_orientation;
    Vec3 previous_driver_velocity;
    Vec3 previous_driver_angular_velocity;
    Vec3 filtered_acceleration;
    Vec3 filtered_angular_acceleration;
    f32 impact = 0;
    bool initialized = false;
    bool active = false;
  };

  void Resolve(const asset::Skeleton &skeleton);

  base::Vector<BodyRegionConfig> regions_;
  base::Vector<State> states_;
  base::Vector<Mat4> model_;
  u64 skeleton_hash_ = 0;
  u32 skeleton_bones_ = 0;
  bool resolved_ = false;
  f32 teleport_distance_ = 0.5f;
  f32 max_acceleration_ = 80.0f;
  f32 model_units_per_metre_ = 1.0f;
};

// Adds body-dynamics output to a dense mesh morph set, resolving source names
// through MorphTarget::name_hash. Existing animation/expression weights are
// preserved and the result is clamped to [min_weight, max_weight].
RX_ANIM_EXPORT void ApplyBodyMorphWeights(
    const asset::Mesh &mesh, const base::Vector<BodyMorphWeight> &body,
    base::Vector<f32> *dense, f32 min_weight = 0, f32 max_weight = 1);

} // namespace rx::anim

#endif // RX_ANIM_BODY_DYNAMICS_H_
