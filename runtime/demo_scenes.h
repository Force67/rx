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
#include "demo_ship.h"
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

  // Releases demo-owned GPU resources (the scenehook demo's raw pipelines) while
  // the renderer's device is still alive. Call from the app's OnShutdown.
  void Shutdown();

 private:
  void CreateWaterDemoScene();
  void CreateMaterialDemoScene();
  void CreateGaussianDemoScene();
  void CreateLodDemoScene();
  void CreateCornellDemoScene();
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

  // --demo bubbles: the streaming-bubble interest map driven locally (no
  // transport), plus its wire-sphere visualizer. Non-null only for that demo.
  bool bubbles_enabled_ = false;
  net::InterestMap bubble_map_;
  std::unique_ptr<net::BubbleVisualizer> bubble_viz_;
  u64 bubble_tick_ = 0;
};

}  // namespace rx

#endif  // RX_RUNTIME_DEMO_SCENES_H_
