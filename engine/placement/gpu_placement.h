#ifndef RX_PLACEMENT_GPU_PLACEMENT_H_
#define RX_PLACEMENT_GPU_PLACEMENT_H_

#include <base/containers/vector.h>

#include "core/export.h"
#include "core/types.h"
#include "placement/placement.h"
#include "render/rhi/device.h"

namespace rx::placement {

// The three-stage GPU placement pipeline (DENSITYMAP -> GENERATE ->
// PLACEMENT). Work is batched by stage across every tile generated in a
// frame - one barrier between stages instead of one round trip per tile -
// and the finished instances land in a host-visible buffer the CPU harvests
// once that frame slot's fence has passed, then feeds to the renderer as
// instance groups.
class RX_PLACEMENT_GPU_EXPORT GpuPlacement {
 public:
  bool Initialize(render::Device& device, const PlacementSystem& system);
  void Shutdown(render::Device& device);

  // Uploads WorldData maps whose revision changed since the last sync (the
  // first call uploads everything). Call outside a recording frame.
  void SyncWorldData(render::Device& device, const WorldData& world);

  // Takes up to max_jobs_per_update tiles from system.pending(), marks them
  // in flight and records the batched three-stage pipeline into `cmd` (the
  // current frame slot's list). Results become readable once this slot's
  // fence passes, i.e. the next time the slot comes around.
  void RecordJobs(render::CommandList& cmd, PlacementSystem& system, u32 slot);

  // Harvests the instances recorded on `slot` frames-in-flight ago; call
  // right after the slot's fence wait (frame begin), before RecordJobs.
  // Completed tiles are marked live; their instances are appended to `out`.
  void Consume(u32 slot, PlacementSystem& system, base::Vector<PlacedInstance>& out,
               base::Vector<TileKey>& out_tiles);

  // Synchronous whole-pipeline run for tests and initial world fill: submits
  // the batch through ImmediateSubmit and reads the results back before
  // returning. Uses a reserved buffer set, safe while frames are idle.
  void GenerateImmediate(render::Device& device, PlacementSystem& system,
                         std::span<const TileKey> tiles, base::Vector<PlacedInstance>& out);

  bool initialized() const { return initialized_; }

 private:
  // One buffer arena per frame slot plus one reserved for immediate mode.
  static constexpr u32 kBufferSets = render::Device::kMaxFramesInFlight + 1;

  struct BufferSet {
    render::GpuBuffer density;    // device local, kMaxJobs cumulative slabs
    render::GpuBuffer points;     // device local, oriented point staging
    render::GpuBuffer counts;     // host visible, single u32
    render::GpuBuffer instances;  // host visible result arena
    base::Vector<TileKey> jobs;
  };

  void RecordBatch(render::CommandList& cmd, const PlacementSystem& system,
                   std::span<const TileKey> tiles, BufferSet& set);
  void ReadResults(const BufferSet& set, base::Vector<PlacedInstance>& out) const;

  bool initialized_ = false;
  u32 max_jobs_ = 0;
  u32 point_capacity_ = 0;

  render::PipelineHandle density_pipeline_;
  render::PipelineHandle generate_pipeline_;
  render::PipelineHandle transform_pipeline_;

  render::GpuBuffer ops_;      // concatenated density bytecode
  render::GpuBuffer layers_;   // PlacementLayerGpu array
  render::GpuBuffer pattern_;  // ordered dither pattern positions

  render::GpuImage world_maps_;
  render::SamplerHandle world_sampler_;
  base::Vector<u64> synced_revisions_;
  u32 synced_map_count_ = 0;

  BufferSet sets_[kBufferSets];
};

}  // namespace rx::placement

#endif  // RX_PLACEMENT_GPU_PLACEMENT_H_
