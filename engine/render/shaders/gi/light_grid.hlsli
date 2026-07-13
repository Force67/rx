#ifndef RX_GI_LIGHT_GRID_HLSLI_
#define RX_GI_LIGHT_GRID_HLSLI_

// World-space light grid sampling. Include after declaring the three bound
// resources (see LightGrid in gi/light_grid.h):
//   ConstantBuffer<LightGridParams> lg_grid   -- per-cascade origins + info
//   StructuredBuffer<uint>          lg_counts  -- per-cell light count
//   StructuredBuffer<uint>          lg_ids     -- per-cell light id list
// under whatever register/space the including shader assigns, then iterate with
// LightGridBegin/LightGridLightId. The ids index the same Light structured
// buffer the froxel cluster consumes.

#define RX_LG_CELLS 16u
#define RX_LG_CASCADES 4u
#define RX_LG_MAX_PER_CELL 32u
#define RX_LG_CELLS_PER_CASCADE (RX_LG_CELLS * RX_LG_CELLS * RX_LG_CELLS)

struct LightGridParams {
  float4 cascade[RX_LG_CASCADES];  // xyz snapped origin, w cell size
  uint4 info;                      // x cells/axis, y cascades, z max per cell, w unused
};

// Smallest cascade whose grid contains `pos`; returns false if outside all.
bool LightGridCell(LightGridParams grid, float3 pos, out uint flat_cell) {
  flat_cell = 0u;
  [unroll]
  for (uint c = 0u; c < RX_LG_CASCADES; ++c) {
    float cell_size = grid.cascade[c].w;
    float3 local = (pos - grid.cascade[c].xyz) / cell_size;
    if (all(local >= 0.0) && all(local < float(RX_LG_CELLS))) {
      uint3 cell = (uint3)local;
      flat_cell = c * RX_LG_CELLS_PER_CASCADE +
                  (cell.z * RX_LG_CELLS + cell.y) * RX_LG_CELLS + cell.x;
      return true;
    }
  }
  return false;
}

#endif  // RX_GI_LIGHT_GRID_HLSLI_
