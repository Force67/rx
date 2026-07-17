#ifndef RX_RUNTIME_DEMO_SCENES_H_
#define RX_RUNTIME_DEMO_SCENES_H_

#include <memory>
#include <vector>

#include <base/containers/vector.h>

#include "anim/anim_graph.h"
#include "anim/foot_placement.h"
#include "anim/pose.h"
#include "anim/rig_player.h"
#include "asset/skeleton.h"
#include "core/math.h"
#include "engine_context.h"
#include "net/bubble.h"
#include "net/bubble_debug.h"
#include "render/core/renderer.h"
#include "demo_nav.h"
#include "demo_placement.h"
#include "demo_ship.h"
#include "demo_gym.h"
#include "demo_puppet.h"
#include "demo_drive.h"
#include "scene_hook_demo.h"
#include "scene_hook_rhi_demo.h"

namespace rx {

// Builds the engine's standalone demo / bring-up scenes (selected by
// --demo-scene) and owns the CPU-side effect state (the particle fountain, the
// gaussian splats, the oit/point-light instance lists) those scenes set up.
// EmitToView feeds that state into the per-frame render view.
class DemoScenes {
 public:
  explicit DemoScenes(EngineContext& ctx);

  // Dispatches on config.demo_scene; the default spins a cube.
  void CreateDemoScene();
  // Appends the live demo effects (particles, gaussians, oit, lights, fur, gpu
  // particles) into this frame's render view.
  void EmitToView(f32 dt, render::FrameView& view);

  // The character/inventory gym (--demo gym), or null for any other scene. The
  // gym drives its own camera + input, so the Viewer routes OnUpdate to it.
  GymDemo* gym() { return gym_.get(); }
  // The locomotion puppet (--demo puppet), or null. Keeps the free-fly camera;
  // the Viewer forwards raw keys to it (1/2/3) without an early return.
  PuppetDemo* puppet() { return puppet_.get(); }
  // The driving gym (--demo drive), or null for any other scene. Like the gym it
  // owns its camera + input, so the Viewer routes OnUpdate to it.
  DriveDemo* drive() { return drive_.get(); }
  // Reapplies demo-specific renderer constraints after the debug UI changes
  // settings. Most demos have none; cloth requires the raster skinning path.
  void ApplyRenderPolicy();

  // Releases demo-owned GPU resources (the scenehook demo's raw pipelines) while
  // the renderer's device is still alive. Call from the app's OnShutdown.
  void Shutdown();

 private:
  void CreateWaterDemoScene();
  // Weather demo: shelter + blocks scene for volumetric precipitation, sky
  // occlusion (dry under the roof) and surface wetness / snow cover.
  void CreateWeatherDemoScene();
  void CreateMaterialDemoScene();
  void CreateGaussianDemoScene();
  void CreateLodDemoScene();
  void CreateCornellDemoScene();
  void CreateInteriorDemoScene();
  void CreateGpuParticleDemoScene();
  void CreateFurDemoScene();
  void CreateAutoLodDemoScene();
  void CreateMaterialXDemoScene();
  void CreateOitDemoScene();
  void CreateOcclusionDemoScene();
  void CreateMeshletDemoScene();
  void CreatePointLightDemoScene();
  void UpdateParticles(f32 dt, render::FrameView& view);
  void CreateFireDemoScene();
  void CreateBrickDemoScene();
  void CreateSilhouettePomDemoScene();
  void CreateVirtualTextureDemoScene();
  void CreateVirtualGeometryDemoScene();
  void CreateStrandHairDemoScene();
  void CreateClothDemoScene();
  void EmitCloth(render::FrameView& view);
  void CreateImposterDemoScene();
  void CreateSssDemoScene();
  void CreateLocomotionDemoScene();
  void EmitLocomotion(f32 dt, render::FrameView& view);
  // Bring-your-own-GPU-passes acceptance demo (--demo scenehook).
  void CreateSceneHookDemoScene();
  // Pure-RHI GPU-driven acceptance demo (--demo scenehook-rhi).
  void CreateSceneHookRhiDemoScene();
  // Streaming-bubble acceptance demo (--demo bubbles): wandering fake players
  // with interest bubbles over a field of replicated entities, entities tinted
  // by their owning peer, wire spheres via the net_viz visualizer.
  void CreateBubbleDemoScene();
  void EmitBubbles(render::FrameView& view);
  // Weather demo thunderstorm: schedules lightning strikes (the game's role;
  // rx renders the bolt + flash light) and drives the global flash scalar with
  // the strike envelope so global and positioned flash agree.
  void UpdateStorm(f32 dt);
  // Water demo: each floating cube pushes a wake ripple + foam splat into the
  // persistent water field, scaled by its physics velocity.
  void EmitWaterDisturbances(f32 dt, render::FrameView& view);
  // Additive spray burst for a slam impact, spawned into the demo particle pool.
  void SpawnSplashSpray(const Vec3& pos, f32 surface, f32 strength);

  struct DemoParticle {
    Vec3 position;
    Vec3 velocity;
    f32 life = 0;
    f32 max_life = 1;
    f32 size = 0.1f;
    Vec3 color;
  };

  EngineContext& ctx_;
  ecs::World& world_;
  ecs::Scheduler& scheduler_;
  render::Renderer& renderer_;
  FlyCamera& camera_;
  physics::PhysicsWorld& physics_;
  const EngineConfig& config_;

  bool particles_enabled_ = false;
  Vec3 particle_emitter_{0, 0, 0};
  // Water demo floaters, tracked to derive per-frame velocity for wake/foam.
  base::Vector<physics::BodyId> water_cubes_;
  base::Vector<Vec3> water_cube_prev_;
  base::Vector<f32> water_cube_slam_cd_;  // per-cube splash cooldown (s)
  f32 water_time_ = 0;                    // wave clock for the Gerstner proxy
  // Weather demo thunderstorm scheduler state.
  bool storm_enabled_ = false;
  bool weather_scene_ = false;  // weather demo active: re-clamp its storm sun
  f32 storm_time_ = 0;
  f32 storm_next_strike_ = 0;
  u32 gpu_particle_count_ = 0;  // > 0 selects the gpu-simulated fountain
  Vec3 gpu_particle_emitter_{0, 0, 0};
  base::Vector<render::Decal> demo_decals_;
  u32 gpu_particle_mode_ = 0;         // 1 = fire (buoyant flames + embers)
  f32 gpu_particle_radius_ = 0.3f;
  f32 gpu_particle_intensity_ = 1.0f;
  f32 fire_time_ = 0.0f;  // drives the campfire light flicker
  bool fur_ball_ = false;
  Vec3 fur_position_{0, 0, 0};
  base::Vector<render::WboitInstance> oit_instances_;
  base::Vector<DemoParticle> demo_particles_;
  base::Vector<render::GaussianInstance> demo_gaussians_;
  base::Vector<render::PointLight> demo_lights_;
  u32 particle_seed_ = 0x9e3779b9u;
  f32 particle_spawn_accum_ = 0;
  f32 demo_input_time_ = 0;

  base::Vector<u32> hair_grooms_;      // strand-hair demo groom handles
  base::Vector<physics::StrandGroomId> hair_sims_;  // matching physics grooms
  u32 hair_orbit_groom_ = 0;           // the groom driven on a slow orbit
  physics::StrandGroomId hair_orbit_strands_ = 0;
  Vec3 hair_orbit_center_{0, 0, 0};
  f32 hair_time_ = 0;

  // --demo cloth: the simulation cage is uploaded once as a skinned mesh;
  // per-vertex palette frames pose it from PhysicsWorld readback each frame.
  physics::ClothId cloth_ = 0;
  u64 cloth_mesh_ = 0;
  u32 cloth_width_ = 0;
  base::Vector<u32> cloth_indices_;
  base::Vector<Vec3> cloth_positions_;
  base::Vector<Vec3> cloth_normals_;
  base::Vector<render::DebugLine> cloth_lines_;

  // Locomotion demo: a kinema-driven skinned biped. The archetype graph is
  // shared; the rig player + foot placement hold this one character's state. The
  // pose/palette buffers are reused every frame (no per-frame allocation).
  bool locomotion_enabled_ = false;
  asset::Skeleton loco_skeleton_;
  asset::SkinBinding loco_skin_;
  u64 loco_mesh_ = 0;
  anim::AnimGraph loco_graph_;
  anim::RigPlayer loco_rig_;
  anim::FootPlacement loco_feet_;
  anim::SkeletonPose loco_pose_;
  base::Vector<i32> loco_remap_;
  base::Vector<Mat4> loco_bone_model_;
  base::Vector<Mat4> loco_palette_;
  Vec3 loco_pos_{0, 0, 0};
  f32 loco_yaw_ = 0;
  f32 loco_time_ = 0;
  u32 loco_footsteps_ = 0;
  Mat4 loco_prev_xform_ = Mat4::Identity();

  // --demo scenehook: an app-owned raw-Vulkan GPU pass recorded through rx's
  // scene hooks. Non-null only for that demo.
  std::unique_ptr<SceneHookDemo> scene_hook_;
  // --demo scenehook-rhi: the same acceptance scene driven purely through the
  // backend-agnostic RHI. Non-null only for that demo.
  std::unique_ptr<SceneHookRhiDemo> scene_hook_rhi_;

  // --demo ship: the procedural sailing-ship slice (hull/sails/rigging/cannons
  // on the ocean stack). Non-null only for that demo.
  std::unique_ptr<ShipDemo> ship_;

  // --demo nav: the navigation chase slice (cost-aware pathfinding over rough
  // terrain, porter vs mule pack). Non-null only for that demo.
  std::unique_ptr<NavDemo> nav_;

  // --demo placement: GPU procedural placement (density programs + ordered
  // dithering streaming a forest around the camera). Non-null only for that
  // demo.
  std::unique_ptr<PlacementDemo> placement_;

  // --demo gym: the character/inventory reference gym (graybox + tuning panel).
  // Non-null only for that demo; the Viewer drives its Update from OnUpdate.
  std::unique_ptr<GymDemo> gym_;

  // --demo puppet: the physics-first locomotion proving ground (graybox arena +
  // rx::locomotion ragdoll + debug overlay). Non-null only for that demo.
  std::unique_ptr<PuppetDemo> puppet_;

  // --demo drive: the GTA-style driving gym (car/boat/plane on mixed terrain).
  // Non-null only for that demo; the Viewer drives its Update from OnUpdate.
  std::unique_ptr<DriveDemo> drive_;

  // --demo bubbles: the streaming-bubble interest map driven locally (no
  // transport), plus its wire-sphere visualizer. Non-null only for that demo.
  bool bubbles_enabled_ = false;
  net::InterestMap bubble_map_;
  std::unique_ptr<net::BubbleVisualizer> bubble_viz_;
  u64 bubble_tick_ = 0;
};

}  // namespace rx

#endif  // RX_RUNTIME_DEMO_SCENES_H_
