#ifndef RX_RUNTIME_DEMO_DRIVE_H_
#define RX_RUNTIME_DEMO_DRIVE_H_

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "audio/vehicle_audio.h"
#include "core/input.h"
#include "core/input_actions.h"
#include "core/math.h"
#include "engine_context.h"
#include "physics/aircraft.h"
#include "physics/boat.h"
#include "physics/physics_world.h"
#include "render/core/renderer.h"

namespace rx {

// GTA-style driving gym (--demo drive): a car, a boat and a plane share one
// ~400x400 m heightfield with mixed surfaces (asphalt road loop + runway, an
// ice patch, dirt and sand) and a lake in one quadrant. Tab cycles the active
// vehicle; the camera + input follow it and the inactive vehicles idle (the car
// holds its brakes). It exercises engine/physics' wheeled-vehicle, boat and
// aircraft simulators, the per-surface tire grip of the material heightfield,
// and engine/audio's procedural VehicleAudio, and shows the vendor test models
// (CesiumMilkTruck as the car, Cesium_Air as the plane, ToyCar/CarConcept/
// GroundVehicle as parked material showcase pieces) through the PBR pipeline.
//
// Like the gym it owns its own camera + input: the Viewer routes OnUpdate here
// and reads the resolved camera back through Emit. Every vendor .glb is
// optional and guarded, so the scene degrades to graybox boxes/cylinders when
// assets/vehicles/ is empty.
class DriveDemo {
 public:
  explicit DriveDemo(EngineContext& ctx);

  // Builds the terrain, water, three vehicles, their audio and the vendor
  // models, and registers the fixed-step vehicle stepping on the scheduler.
  void Create();

  // Render-cadence input + camera + audio. Captures this frame's driver input
  // into members the fixed-step StepVehicles consumes, drives the chase / free
  // camera, and pushes telemetry to the three VehicleAudio voices.
  void Update(f32 dt, const InputState& input, const ActionState& actions, bool allow_keyboard,
              bool allow_mouse);

  // Writes the resolved camera, the vehicle + showcase draws (car/plane models,
  // suspension wheels, procedural boat hull) and the HUD panel into the view.
  void Emit(f32 dt, render::FrameView& view);

  // The drive demo keeps the OS cursor free so the debug UI stays clickable.
  bool wants_mouse_capture() const { return false; }

 private:
  enum class Vehicle : u32 { kCar, kBoat, kPlane, kCount };

  // A loaded vendor model flattened to (mesh hash, model-local transform) parts,
  // plus its model-space AABB for scale-to-fit framing. `loaded` is false when
  // the .glb was absent, so callers fall back to graybox geometry.
  struct Model {
    bool loaded = false;
    std::vector<std::pair<u64, Mat4>> parts;
    Vec3 aabb_min{0, 0, 0};
    Vec3 aabb_max{0, 0, 0};
  };

  void BuildTerrain();
  void BuildWater();
  void BuildBoatHull();
  void BuildWheelMesh();
  void SpawnVehicles();
  void SetupAudio();

  // Loads a .glb through the same path as --gltf. `skip_mesh_index` drops parts
  // sourced from that glTF mesh (the truck's rigid wheel mesh, replaced by the
  // suspension-driven wheel cylinders). Returns false (and leaves out->loaded
  // false) when the file is missing.
  bool LoadModel(const std::string& path, i32 skip_mesh_index, Model* out);
  // Recenter + scale-to-fit + yaw a model into a unit placement frame, so the
  // final draw is `place * NormalizeXform(...) * part.local`. sit_on_ground puts
  // the model's min-Y at the frame origin (parked pieces); else its centre.
  Mat4 NormalizeXform(const Model& m, f32 target_len, f32 model_yaw, bool sit_on_ground) const;
  void EmitModel(render::FrameView& view, const Model& m, const Mat4& xf) const;

  // Fixed-step (kPreSim, before PhysicsWorld::Update): stages this step's input
  // on each vehicle. Cars get DriveVehicle, the boat and plane accumulate their
  // hull/aero forces (their Update contract is "before the world step").
  void StepVehicles(f32 dt);
  void ResetActive();
  // Despawns the car and respawns it with handling profile `index` (0..5) at its
  // current position/heading (stationary). Number keys 1-6 call this live.
  void SetCarProfile(u32 index);
  // Despawns the boat and respawns it with boat-type profile `index` (0..4) at
  // its current pose (re-settling on the lake), re-applying the current cargo and
  // swapping the audio voice. Keys 1-5 call this live while the boat is active.
  void SetBoatProfile(u32 index);
  // Applies cargo `frac` (of the profile's max_cargo_kg; > 1 overloads) to the
  // live boat via Boat::SetCargo, so it visibly settles deeper. Key L cycles it.
  void SetBoatCargo(f32 frac);
  void UpdateChaseCamera(f32 dt, const InputState& input, const ActionState& actions,
                         bool allow_keyboard, bool allow_mouse);
  void DrawPanel();

  // Aggregate max longitudinal wheel slip of the car (for the skid audio layer).
  f32 CarMaxSlip() const;

  EngineContext& ctx_;

  // --- vehicles ---
  physics::VehicleId car_ = 0;
  std::unique_ptr<physics::Boat> boat_;
  std::unique_ptr<physics::Aircraft> aircraft_;
  Vehicle active_ = Vehicle::kCar;

  // Active handling profile (0..5: sports/muscle/hatch/suv/van/semi) and the
  // desc it resolved to, kept so a reset respawns the same profile and Emit can
  // scale the graybox chassis to the desc dimensions.
  u32 car_profile_ = 0;
  physics::PhysicsWorld::VehicleDesc car_desc_{};
  bool car_truckish_ = false;  // draw the milk-truck glTF (van/semi) vs a box
  u32 car_tint_ = 0xC03828;    // graybox chassis tint for car-shaped profiles

  Vec3 car_spawn_{48, 1, -30};
  f32 car_yaw_ = 0;
  Vec3 boat_spawn_{120, -1.4f, 120};
  f32 boat_yaw_ = 0;
  Vec3 plane_spawn_{-62, 1.4f, -140};
  f32 plane_yaw_ = 0;

  // --- captured driver input (render cadence -> fixed-step StepVehicles) ---
  f32 car_throttle_ = 0;   // -1..1 forward/reverse gas
  f32 car_steer_ = 0;      // -1..1
  f32 car_brake_ = 0;      // 0..1
  f32 car_handbrake_ = 0;  // 0..1
  bool car_manual_ = false;
  bool shift_up_pending_ = false;
  bool shift_down_pending_ = false;

  f32 boat_throttle_ = 0;
  f32 boat_steer_ = 0;

  // Active boat-type profile (0..4: dinghy/speedboat/jetski/fishing/barge), the
  // desc it resolved to (for the hull-scale + HUD) and the current cargo fraction
  // cycled by key L (0 / 0.5 / 1.0 / 1.25-overload).
  u32 boat_profile_ = 1;  // speedboat by default (the BoatDesc defaults)
  physics::BoatDesc boat_desc_{};
  u32 boat_cargo_step_ = 0;  // 0/1/2/3 -> 0 / 0.5 / 1.0 / 1.25
  f32 boat_cargo_frac_ = 0;

  f32 plane_throttle_ = 0;  // persistent 0..1
  f32 plane_pitch_ = 0;
  f32 plane_roll_ = 0;
  f32 plane_rudder_ = 0;
  f32 plane_brakes_ = 0;
  u32 flap_step_ = 0;  // 0/1/2 -> 0 / 0.5 / 1
  f32 plane_flaps_ = 0;

  u32 wetness_step_ = 0;  // 0/1/2 -> 0 / 0.5 / 1
  f32 wetness_ = 0;

  // --- procedural audio (one voice set per vehicle) ---
  std::unique_ptr<audio::VehicleAudio> car_audio_;
  std::unique_ptr<audio::VehicleAudio> boat_audio_;
  std::unique_ptr<audio::VehicleAudio> plane_audio_;

  // --- visuals ---
  Model car_model_;
  Model plane_model_;
  std::vector<std::pair<Model, Mat4>> showcase_;  // parked pieces + baked placement
  Mat4 car_norm_ = Mat4::Identity();
  Mat4 plane_norm_ = Mat4::Identity();
  u64 wheel_mesh_ = 0;    // engine-drawn suspension wheel cylinder
  u64 boat_mesh_ = 0;     // procedural painted hull (demo_ship style)
  u64 graybox_car_ = 0;   // fallback chassis box when the truck .glb is absent
  u64 graybox_plane_ = 0;  // fallback fuselage box when the air .glb is absent
  f32 car_wheel_radius_ = 0.34f;
  f32 car_wheel_width_ = 0.25f;

  // --- camera ---
  bool free_cam_ = false;
  bool cam_init_ = false;
  Vec3 cam_eye_{0, 4, -40};
  Vec3 cam_target_{0, 1, 0};
  f32 cam_fov_ = 1.0472f;

  bool registered_ = false;  // scheduler system installed once
};

}  // namespace rx

#endif  // RX_RUNTIME_DEMO_DRIVE_H_
