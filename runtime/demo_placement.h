#ifndef RX_RUNTIME_DEMO_PLACEMENT_H_
#define RX_RUNTIME_DEMO_PLACEMENT_H_

#include <base/containers/vector.h>

#include "core/math.h"
#include "engine_context.h"
#include "placement/gpu_placement.h"
#include "placement/placement.h"
#include "placement/world_data.h"
#include "render/core/renderer.h"

namespace rx {

// GPU procedural placement demo (--demo placement): a 1 km^2 landscape whose
// natural content - pines, broadleaves, dead trees, bushes, rocks, grass - is
// not stored anywhere. WorldData maps (height, forest, road, water) plus
// per-layer density programs describe the world; the placement pipeline
// discretizes them into instances in rings around the camera, on the GPU
// while flying (results harvested a frame later and fed to the renderer as
// per-tile instance groups), with layered dithering keeping same-footprint
// assets from colliding. Fly through the forest: tiles stream in and out
// deterministically - leave and return, and every tree is where it was.
// RX_PLACEMENT_LINES=1 overlays the streaming tile bounds.
class PlacementDemo {
 public:
  explicit PlacementDemo(EngineContext& ctx);
  ~PlacementDemo();

  // Builds terrain + WorldData + ecotopes, uploads the hand-authored meshes,
  // and synchronously fills the initial rings around the spawn point.
  void Create();

  // Streams tiles: harvests results recorded frames-in-flight ago, retires
  // tiles the camera left, and hooks this frame's placement compute batch
  // into the scene. Called from DemoScenes::EmitToView.
  void Emit(f32 dt, render::FrameView& view);

  // Releases the GPU pipeline resources before the renderer dies.
  void Shutdown();

 private:
  f32 TerrainHeight(f32 x, f32 z) const;
  void BuildWorldData();
  void BuildEcotopes();
  void BuildTerrainMesh();
  void ApplyResults(std::span<const placement::PlacedInstance> instances);
  void DestroyTileGroups(const placement::TileKey& key);

  EngineContext& ctx_;
  placement::WorldData world_;
  std::unique_ptr<placement::PlacementSystem> system_;
  placement::GpuPlacement gpu_;
  bool gpu_ready_ = false;

  u32 map_height_ = 0;
  u32 map_forest_ = 0;
  u32 map_road_ = 0;
  u32 map_water_ = 0;

  base::Vector<u64> layer_meshes_;  // flat-layer index -> renderer mesh id
  base::Vector<u32> layer_stacks_;  // flat-layer index -> stack index

  struct TileGroups {
    placement::TileKey key;
    base::Vector<render::InstanceGroupHandle> groups;
  };
  base::Vector<TileGroups> live_;

  // Filled by the scene hook (Consume), turned into instance groups on the
  // next Emit - renderer calls stay outside command recording.
  base::Vector<placement::PlacedInstance> harvested_;
  base::Vector<placement::TileKey> harvested_tiles_;

  base::Vector<render::DebugLine> lines_;
  bool draw_lines_ = false;
  bool autopilot_ = false;
  f32 fly_time_ = 0.0f;
};

}  // namespace rx

#endif  // RX_RUNTIME_DEMO_PLACEMENT_H_
