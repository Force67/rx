#ifndef RX_RENDER_DECAL_BAKE_H_
#define RX_RENDER_DECAL_BAKE_H_

// Texture-space decal baking. A projected decal is rasterized ONCE into a small
// per-instance layer that lives in the receiver's UV space, and the forward
// pass composites that layer over the material with a single extra texture
// fetch. Unlike the clustered projector path (mesh_pipeline.h Decal), the cost
// of a stamped decal does not grow with the number of decals: a character
// carrying two hundred blood splats shades exactly as fast as a clean one.
//
// The layers are tiles in three shared atlases, so a receiver costs a fixed
// tile no matter how much gets thrown at it, and nothing at all until its
// first stamp:
//   albedo  RGBA8  premultiplied decal colour + coverage
//   fx      RGBA8  tangent-space normal xy, roughness multiplier, coverage
//   chart   R8     1 where the receiver's UV charts are, for the gutter fill
// A 2D decal (a tattoo) writes albedo only; a "3D fx" decal (wet splatter,
// embossed ink) also perturbs the normal and the roughness, so it catches the
// light like a real surface feature.
//
// Tiles are recycled LRU. Every stamp is also journalled CPU-side (~112 bytes),
// so a receiver whose tile was evicted while it was off screen REBAKES its
// whole history in one draw the next time it appears - the decals survive
// without the memory. That is the whole trade: keep the cheap description,
// throw away the expensive pixels.
//
// Requirements on a receiver: its lod-0 UV0 must be unique across the mesh
// (charts must not overlap). Character and prop UVs normally are; tiling
// architecture UVs are not, and a decal stamped on one would repeat across
// every tile. One tile covers the whole mesh, so submeshes that each re-use the
// full 0..1 UV space share it.
//
// A receiver's uvs do not have to BE 0..1, though. SetReceiverUv applies a
// scale+bias first, which is what makes UDIM content work: a Daz/Genesis figure
// lays its body zones out across u in [0,7), and a receiver biased onto one of
// those tiles gets the whole layer to itself at full resolution. Geometry that
// falls outside 0..1 after the transform takes no decal at all, in the bake and
// in the forward pass alike.

#include <span>

#include <base/containers/vector.h>

#include "core/export.h"
#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/pipeline/mesh_pipeline.h"
#include "render/rhi/device.h"

namespace rx::render {

// A decal to bake into a receiver's texture space. The projector is the same
// oriented box the clustered path uses, so the same authored atlas page can be
// thrown either way. params2.z (clustered emissive) is not supported here:
// glowing decals stay on the projector path, which is cheap for a handful.
struct DecalStamp {
  u32 receiver = 0;  // DecalBaker handle; 0 is ignored
  Decal projector;
};

// Projector box for a decal facing along `normal` (the receiver's outward
// surface normal), oriented so `up` runs along the decal's +v axis. `width` /
// `height` are the full extents in the projection plane and `depth` how far
// along the normal the box accepts geometry, all in meters.
RX_RENDER_EXPORT Decal MakeDecalProjector(const Vec3& position, const Vec3& normal,
                                          const Vec3& up, f32 width, f32 height, f32 depth);

class DecalBaker {
 public:
  // The push-constant channel that carries the tile to the forward pass is one
  // byte wide (see Renderer's tint_packed packing), and 0 means "no layer".
  static constexpr u32 kMaxTiles = 255;
  static constexpr u32 kMaxFrameStamps = 256;

  struct Desc {
    // Atlas edge in texels; tile_size must divide it. atlas_size/tile_size
    // squared tiles fit, capped at kMaxTiles. The default holds 16 receivers at
    // 256^2 each for ~11 MB; a character-heavy game wants 2048/512 (~47 MB).
    // RX_DECAL_ATLAS / RX_DECAL_TILE override both without a rebuild.
    u32 atlas_size = 1024;
    u32 tile_size = 256;
    // Stamps replayed when a tile is rebaked. Older stamps past this fall off
    // the front, which is invisible in practice: later splats cover them.
    u32 journal_limit = 48;
  };

  // A receiver drawn this frame, with the geometry and pose to bake against.
  // Skinned receivers bake in their CURRENT pose, so a splat lands where it hit
  // and then rides the animation.
  struct Target {
    u32 receiver = 0;
    const GpuMesh* mesh = nullptr;
    Mat4 transform = Mat4::Identity();
    const GpuBuffer* bones = nullptr;  // frame bone palette, null = static
    u32 skin_offset = 0;
  };

  struct Stats {
    u32 receivers = 0;      // live handles
    u32 resident_tiles = 0;
    u32 tile_capacity = 0;
    u32 journalled = 0;     // stamps held across every receiver
    u32 bakes = 0;          // draws recorded last frame
    // Lifetime tiles taken from one receiver and handed to another. Each one
    // owes a journal replay the next time the loser draws, so this is the
    // number to watch when sizing the atlas.
    u32 evictions = 0;
    u64 bytes = 0;          // atlas footprint, mip 0
  };

  // A Desc with the RX_DECAL_ATLAS / RX_DECAL_TILE overrides applied. Only the
  // no-desc Initialize consults them: code that sizes the layers explicitly
  // means it.
  static Desc EnvDesc();
  bool Initialize(Device& device, const Desc& desc);
  bool Initialize(Device& device) { return Initialize(device, EnvDesc()); }
  void Destroy(Device& device);
  bool available() const { return static_cast<bool>(stamp_pipeline_); }

  u32 AcquireReceiver();
  void ReleaseReceiver(u32 receiver);
  // Maps the receiver's uvs into its layer tile: layer = uv * scale + bias.
  // Identity by default. Repaints the tile, since the mapping changes where
  // every already-baked stamp lands.
  void SetReceiverUv(u32 receiver, f32 scale_u, f32 scale_v, f32 bias_u, f32 bias_v);
  // Queues a stamp against a receiver; it bakes the next frame the receiver
  // draws. False when the handle is unknown.
  bool Stamp(const DecalStamp& stamp);
  // Washes a receiver clean: drops the journal and repaints the tile.
  void ClearReceiver(u32 receiver);

  // Tile the forward pass should sample for this receiver, biased by one so 0
  // reads as "no layer". Valid only after AddToGraph has run for the frame.
  u32 tile_slot(u32 receiver) const;

  // Assigns tiles and records this frame's bakes. `targets` are the receivers
  // drawn this frame (the renderer collects them from the draw list); a
  // receiver that is not drawn keeps its queued stamps until it is.
  // `source_albedo` / `source_normal` are the authored decal atlas the stamps'
  // uv_rect indexes; either may be null, and a built-in white / flat page
  // stands in.
  void AddToGraph(RenderGraph& graph, std::span<const Target> targets, u32 frame_slot,
                  u64 frame_index, TextureView source_albedo, TextureView source_normal);

  TextureView albedo_view() const { return albedo_.view; }
  TextureView fx_view() const { return fx_.view; }
  // Per-tile uv scale+bias (float4 each), indexed by tile. The forward pass
  // needs it to reproduce the mapping the bake used.
  const GpuBuffer& tile_uv_buffer() const { return tile_uv_xform_; }
  // The backing atlas, for readback and debug views. Left in
  // kShaderReadFragment by every bake.
  const GpuImage& albedo_atlas() const { return albedo_; }
  const GpuImage& fx_atlas() const { return fx_; }
  // FrameGlobals::decal_layer: x tiles per atlas row, y tile edge in atlas uv.
  f32 tiles_per_row() const { return static_cast<f32>(tiles_per_row_); }
  f32 tile_uv() const { return tile_uv_; }
  const Stats& stats() const { return stats_; }

 private:
  // One stamp as the bake pixel shader reads it. The projector rows plus the
  // atlas page and the blend/fx weights; 96 bytes, float4 rows so the
  // StructuredBuffer stride matches without padding.
  struct GpuStamp {
    f32 row0[4];
    f32 row1[4];
    f32 row2[4];
    f32 uv_rect[4];
    f32 tint_blend[4];
    f32 params2[4];
  };
  static_assert(sizeof(GpuStamp) == 96);

  struct BakePush {
    Mat4 model;
    u32 first_stamp = 0;
    u32 stamp_count = 0;
    u32 skin_offset = 0;
    u32 pad = 0;
    f32 uv_scale[2] = {1, 1};
    f32 uv_bias[2] = {0, 0};
  };

  struct Receiver {
    bool alive = false;
    u32 tile = kNoTile;
    u64 last_used = 0;
    f32 uv_scale[2] = {1, 1};
    f32 uv_bias[2] = {0, 0};
    // Stamps not yet in the tile. A rebake moves the whole journal here.
    base::Vector<GpuStamp> pending;
    base::Vector<GpuStamp> journal;
    bool repaint = false;  // tile content is stale: clear then replay
  };

  static constexpr u32 kNoTile = 0xffffffffu;

  bool CreateAtlases(Device& device);
  bool CreatePipelines(Device& device);
  // Picks a free tile, or evicts the coldest receiver that is not drawing this
  // frame. kNoTile when every tile is in use by a live target.
  u32 AcquireTile(u32 receiver, u64 frame_index);
  Receiver* find(u32 receiver);
  const Receiver* find(u32 receiver) const;

  Desc desc_;
  u32 tiles_per_row_ = 0;
  u32 tile_count_ = 0;
  u32 mip_count_ = 1;
  f32 tile_uv_ = 0;

  GpuImage albedo_;  // RGBA8 premultiplied colour + coverage
  GpuImage fx_;      // RGBA8 normal xy, roughness multiplier, coverage
  GpuImage chart_;   // R8 UV-chart mask, drives the gutter fill
  // Mip-0 views: the whole-image views span the chain, which is legal neither
  // as a render target nor as a storage image.
  TextureView albedo_mip0_;
  TextureView fx_mip0_;
  // Neutral tile contents, uploaded to clear a tile before a repaint. Filled
  // once at startup: transparent albedo, flat/unity fx, empty chart.
  GpuBuffer clear_staging_;
  u64 clear_albedo_offset_ = 0;
  u64 clear_fx_offset_ = 0;
  u64 clear_chart_offset_ = 0;
  GpuBuffer stamps_[Device::kMaxFramesInFlight];
  // Stand-ins for a caller with no authored decal atlas: a stamp then paints
  // the projector's own footprint, which is a usable solid decal rather than a
  // null descriptor.
  GpuImage white_;
  GpuImage flat_normal_;
  GpuBuffer tile_uv_xform_;  // host visible float4[kMaxTiles], scale.xy bias.zw

  PipelineHandle stamp_pipeline_;
  PipelineHandle stamp_skin_pipeline_;
  PipelineHandle dilate_pipeline_;
  SamplerHandle sampler_;
  ResourceState atlas_state_ = ResourceState::kUndefined;  // albedo_ + fx_, in lockstep
  ResourceState chart_state_ = ResourceState::kUndefined;

  base::Vector<Receiver> receivers_;  // index + 1 = handle
  base::Vector<u32> free_receivers_;
  base::Vector<u32> tile_owner_;      // tile -> receiver handle, 0 = free
  Stats stats_;
};

}  // namespace rx::render

#endif  // RX_RENDER_DECAL_BAKE_H_
