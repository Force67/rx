#ifndef RX_RENDER_LIGHT_GRID_H_
#define RX_RENDER_LIGHT_GRID_H_

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/device.h"

namespace rx::render {

// Cascaded world-space light grid. Unlike the frustum-tied froxel cluster
// (light_cluster.cs.hlsl), this bins the frame's dynamic lights into a
// camera-following world-space grid so ray hits *outside* the view frustum
// (the radiance cache's shade points) can still be lit.
//
// 16^3 cells x 4 exponential cascades: cascade 0 spans 32 m (2 m cells), each
// further cascade doubles the extent (and the cell size). Origins are snapped
// to the cascade cell size around the camera (no crawling). Binning is a single
// dispatch of 4x4x4 groups: phase 1 coarse-culls all lights against the group's
// world AABB into a groupshared bit array, phase 2 tests survivors against each
// cell's AABB.
//
// Buffer layout (consumed by rcgi_cache_shade and any future world consumer,
// see light_grid.hlsli):
//   params  (UBO)  : per-cascade float4 {origin.xyz, cell_size} + info uint4.
//   counts  (SB u32): flat, cell-major. index = cascade*16^3 + (cz*16+cy)*16+cx.
//   ids     (SB u32): counts[cell] entries at ids[cell * kMaxPerCell + i], each
//                     an index into the bound Light/PointLight structured buffer.
class LightGrid {
 public:
  static constexpr u32 kCells = 16;         // cells per axis
  static constexpr u32 kCascades = 4;
  static constexpr u32 kMaxLights = 256;    // matches the froxel cluster cap
  static constexpr u32 kMaxPerCell = 32;
  static constexpr f32 kCascade0Extent = 32.0f;  // meters across cascade 0

  static constexpr u32 kCellsPerCascade = kCells * kCells * kCells;
  static constexpr u32 kTotalCells = kCellsPerCascade * kCascades;

  bool Initialize(Device& device);
  void Destroy(Device& device);

  // Snaps every cascade to the camera, uploads the per-cascade params, and adds
  // the binning dispatch. `lights` is the same StructuredBuffer<Light> the
  // froxel cluster pass consumes; `light_count` is capped to kMaxLights.
  void AddToGraph(RenderGraph& graph, const GpuBuffer& lights, u32 light_count,
                  const Vec3& camera, u32 frame_index, bool async = false);

  const GpuBuffer& params_buffer(u32 frame_index) const {
    return params_buffers_[frame_index % 2];
  }
  const GpuBuffer& counts_buffer() const { return counts_; }
  const GpuBuffer& ids_buffer() const { return ids_; }
  static u64 params_size() { return sizeof(GridParams); }

 private:
  struct GridParams {
    f32 cascade[kCascades][4];  // xyz snapped origin, w cell size
    u32 info[4];                // x cells/axis, y cascades, z max per cell, w unused
  };

  Device* device_ = nullptr;
  PipelineHandle pipeline_;
  GpuBuffer params_buffers_[2];  // host visible, ping-pong by frame parity
  GpuBuffer counts_;             // device-local storage
  GpuBuffer ids_;                // device-local storage
};

}  // namespace rx::render

#endif  // RX_RENDER_LIGHT_GRID_H_
