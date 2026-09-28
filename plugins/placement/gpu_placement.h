#ifndef RX_PLACEMENT_GPU_PLACEMENT_H_
#define RX_PLACEMENT_GPU_PLACEMENT_H_

#include <base/containers/vector.h>

#include "base/containers/span.h"
#include "foundation/build_config/export.h"
#include "foundation/build_config/types.h"
#include "plugins/placement/placement.h"
#include "rxe/gpu/rhi/device.h"

namespace rx::placement {

// The three-stage GPU placement pipeline (DENSITYMAP -> GENERATE ->
// PLACEMENT). Work is batched by stage across every tile generated in a
// frame - one barrier between stages instead of one round trip per tile -
// and the finished instances land in a host-visible buffer the CPU harvests
// once that frame slot's fence has passed, then feeds to the renderer as
// instance groups.
class RX_PLACEMENT_GPU_EXPORT GpuPlacement {
 public:
  bool Initialize(gpu::Device& device, const PlacementSystem& system);
  void Shutdown(gpu::Device& device);

  // Uploads WorldData maps whose revision changed since the last sync (the
  // first call uploads everything). Call outside a recording frame.
  void SyncWorldData(gpu::Device& device, const WorldData& world);

  // Takes up to max_jobs_per_update tiles from system.pending(), marks them
  // in flight and records the batched three-stage pipeline into `cmd` (the
  // current frame slot's list). Results become readable once this slot's
  // fence passes, i.e. the next time the slot comes around.
  void RecordJobs(gpu::CommandList& cmd, PlacementSystem& system, u32 slot);

  // Harvests the instances recorded on `slot` frames-in-flight ago; call
  // right after the slot's fence wait (frame begin), before RecordJobs.
  // Completed tiles are marked live; their instances are appended to `out`.
  void Consume(u32 slot, PlacementSystem& system, base::Vector<PlacedInstance>& out,
               base::Vector<TileKey>& out_tiles);

  // Synchronous whole-pipeline run for tests and initial world fill: submits
  // the batch through ImmediateSubmit and reads the results back before
  // returning. Uses a reserved buffer set, safe while frames are idle.
  void GenerateImmediate(gpu::Device& device, PlacementSystem& system,
                         base::Span<const TileKey> tiles, base::Vector<PlacedInstance>& out);

  bool initialized() const { return initialized_; }

 private:
  // One buffer arena per frame slot plus one reserved for immediate mode.
  static constexpr u32 kBufferSets = gpu::Device::kMaxFramesInFlight + 1;

  struct BufferSet {
    gpu::GpuBuffer density;    // device local, kMaxJobs cumulative slabs
    gpu::GpuBuffer points;     // device local, oriented point staging
    gpu::GpuBuffer counts;     // host visible, single u32
    gpu::GpuBuffer instances;  // host visible result arena
    base::Vector<TileKey> jobs;
  };

  void RecordBatch(gpu::CommandList& cmd, const PlacementSystem& system,
                   base::Span<const TileKey> tiles, BufferSet& set);
  void ReadResults(const BufferSet& set, base::Vector<PlacedInstance>& out) const;

  bool initialized_ = false;
  u32 max_jobs_ = 0;
  u32 point_capacity_ = 0;

  gpu::PipelineHandle density_pipeline_;
  gpu::PipelineHandle generate_pipeline_;
  gpu::PipelineHandle transform_pipeline_;

  gpu::GpuBuffer ops_;      // concatenated density bytecode
  gpu::GpuBuffer layers_;   // PlacementLayerGpu array
  gpu::GpuBuffer pattern_;  // ordered dither pattern positions

  gpu::GpuImage world_maps_;
  gpu::SamplerHandle world_sampler_;
  base::Vector<u64> synced_revisions_;
  u32 synced_map_count_ = 0;

  BufferSet sets_[kBufferSets];
};

}  // namespace rx::placement

#endif  // RX_PLACEMENT_GPU_PLACEMENT_H_
