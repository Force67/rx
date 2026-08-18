#include "render/texturing/decal_bake.h"

#include <algorithm>
#include <cstring>

#include <base/option.h>

#include "asset/mesh.h"
#include "core/log.h"
#include "shaders/decal_bake_ps_hlsl.h"
#include "shaders/decal_bake_skin_vs_hlsl.h"
#include "shaders/decal_bake_vs_hlsl.h"
#include "shaders/decal_dilate_cs_hlsl.h"

namespace rx::render {
namespace {

// Layer sizing. A character's uv zone has to fit in one tile, so tattoo-grade
// detail wants 512 or 1024 where a splatter-only game is fine at the default.
// Env knobs rather than a build constant: the right size is content-dependent
// and worth trying without a rebuild.
base::Option<int> AtlasSize{"decal.atlas", 0, "RX_DECAL_ATLAS"};
base::Option<int> TileSize{"decal.tile", 0, "RX_DECAL_TILE"};

// Tile edges below this are not worth a mip chain; above it the chain stops at
// 16 texels a side, which is as small as a decal layer stays meaningful.
constexpr u32 kMinMipTexels = 16;

struct DilatePush {
  u32 origin[2];
  u32 size;
  u32 pad;
};

u32 MipCountFor(u32 tile_size) {
  u32 mips = 1;
  while ((tile_size >> mips) >= kMinMipTexels) ++mips;
  return std::min(mips, 5u);
}

void WriteRow(const Vec3& axis, f32 half_extent, const Vec3& origin, f32 out[4]) {
  const f32 inv = half_extent > 1e-6f ? 1.0f / half_extent : 0.0f;
  out[0] = axis.x * inv;
  out[1] = axis.y * inv;
  out[2] = axis.z * inv;
  out[3] = -(axis.x * origin.x + axis.y * origin.y + axis.z * origin.z) * inv;
}

}  // namespace

Decal MakeDecalProjector(const Vec3& position, const Vec3& normal, const Vec3& up, f32 width,
                         f32 height, f32 depth) {
  Decal decal;
  const Vec3 n = Normalize(normal);
  Vec3 tangent = Cross(up, n);
  if (Length(tangent) < 1e-4f) tangent = Cross(Vec3{0, 0, 1}, n);
  if (Length(tangent) < 1e-4f) tangent = Cross(Vec3{1, 0, 0}, n);
  tangent = Normalize(tangent);
  const Vec3 bitangent = Cross(n, tangent);
  WriteRow(tangent, width * 0.5f, position, decal.row0);
  WriteRow(bitangent, height * 0.5f, position, decal.row1);
  WriteRow(n, depth * 0.5f, position, decal.row2);
  return decal;
}

DecalBaker::Desc DecalBaker::EnvDesc() {
  Desc desc;
  if (AtlasSize.get() > 0) desc.atlas_size = static_cast<u32>(AtlasSize.get());
  if (TileSize.get() > 0) desc.tile_size = static_cast<u32>(TileSize.get());
  return desc;
}

bool DecalBaker::Initialize(Device& device, const Desc& desc) {
  desc_ = desc;
  if (desc_.tile_size == 0 || desc_.atlas_size < desc_.tile_size ||
      desc_.atlas_size % desc_.tile_size != 0) {
    RX_WARN("decal baker: atlas {} is not a whole number of {} tiles; layers disabled",
            desc_.atlas_size, desc_.tile_size);
    return false;
  }
  tiles_per_row_ = desc_.atlas_size / desc_.tile_size;
  tile_count_ = std::min(tiles_per_row_ * tiles_per_row_, kMaxTiles);
  tile_uv_ = static_cast<f32>(desc_.tile_size) / static_cast<f32>(desc_.atlas_size);
  mip_count_ = MipCountFor(desc_.tile_size);
  // A rebake replays the whole journal in one frame, so a journal longer than
  // the frame budget could never fit and that receiver would defer forever.
  desc_.journal_limit = std::clamp(desc_.journal_limit, 1u, kMaxFrameStamps);

  if (!CreateAtlases(device) || !CreatePipelines(device)) {
    Destroy(device);
    return false;
  }
  // The forward pass binds the atlases every frame whether or not anything has
  // ever been stamped, so they have to leave startup shader-readable rather
  // than waiting for a first bake that may never come.
  // Content, not just a layout: a draw that reaches the atlas before any tile
  // has been baked (a stale tile index, a clamped uv) would otherwise sample
  // uninitialized VRAM, and an arbitrary alpha defeats the coverage early-out.
  const f32 zero[4] = {0, 0, 0, 0};
  const f32 neutral[4] = {0.5f, 0.5f, 0.5f, 0};
  device.ImmediateSubmit([&](CommandList& cmd) {
    TextureBarrier to_clear[3] = {
        Transition(albedo_, ResourceState::kUndefined, ResourceState::kCopyDst),
        Transition(fx_, ResourceState::kUndefined, ResourceState::kCopyDst),
        Transition(chart_, ResourceState::kUndefined, ResourceState::kCopyDst)};
    cmd.TextureBarriers(to_clear);
    cmd.ClearColor(albedo_, zero);
    cmd.ClearColor(fx_, neutral);
    cmd.ClearColor(chart_, zero);
    TextureBarrier ready[3] = {
        Transition(albedo_, ResourceState::kCopyDst, ResourceState::kShaderReadFragment),
        Transition(fx_, ResourceState::kCopyDst, ResourceState::kShaderReadFragment),
        Transition(chart_, ResourceState::kCopyDst, ResourceState::kShaderReadFragment)};
    cmd.TextureBarriers(ready);
  });
  atlas_state_ = ResourceState::kShaderReadFragment;
  chart_state_ = ResourceState::kShaderReadFragment;

  tile_owner_.resize(tile_count_);
  for (u32 i = 0; i < tile_count_; ++i) tile_owner_[i] = 0;
  stats_.tile_capacity = tile_count_;
  stats_.bytes = static_cast<u64>(desc_.atlas_size) * desc_.atlas_size * (4 + 4 + 1);
  return true;
}

bool DecalBaker::CreateAtlases(Device& device) {
  const Extent2D extent{desc_.atlas_size, desc_.atlas_size};
  const TextureUsageFlags layer_usage = kTextureUsageSampled | kTextureUsageStorage |
                                        kTextureUsageColorTarget | kTextureUsageTransferSrc |
                                        kTextureUsageTransferDst;
  albedo_ = device.CreateImage2D(Format::kRGBA8Unorm, extent, layer_usage, mip_count_);
  fx_ = device.CreateImage2D(Format::kRGBA8Unorm, extent, layer_usage, mip_count_);
  // The chart mask needs no mips: only the dilate reads it, always at mip 0.
  chart_ = device.CreateImage2D(Format::kR8Unorm, extent,
                                kTextureUsageSampled | kTextureUsageColorTarget |
                                    kTextureUsageTransferDst,
                                1);
  if (!albedo_ || !fx_ || !chart_) {
    RX_WARN("decal baker: atlas allocation failed; texture-space decals disabled");
    return false;
  }
  albedo_mip0_ = device.CreateMipView(albedo_, 0);
  fx_mip0_ = device.CreateMipView(fx_, 0);
  if (!albedo_mip0_ || !fx_mip0_) {
    RX_WARN("decal baker: mip view creation failed; texture-space decals disabled");
    return false;
  }

  // One tile's worth of neutral content per atlas, filled once and re-uploaded
  // whenever a tile changes hands. Transparent albedo; fx flat-normal (0.5) and
  // roughness multiplier 1 (encoded as 0.5); empty chart.
  const u64 texels = static_cast<u64>(desc_.tile_size) * desc_.tile_size;
  clear_albedo_offset_ = 0;
  clear_fx_offset_ = texels * 4;
  clear_chart_offset_ = clear_fx_offset_ + texels * 4;
  clear_staging_ = device.CreateBuffer(clear_chart_offset_ + texels, kBufferUsageTransferSrc, true);
  if (!clear_staging_.mapped) {
    RX_WARN("decal baker: clear staging allocation failed; texture-space decals disabled");
    return false;
  }
  u8* bytes = static_cast<u8*>(clear_staging_.mapped);
  std::memset(bytes + clear_albedo_offset_, 0, texels * 4);
  for (u64 i = 0; i < texels; ++i) {
    u8* fx = bytes + clear_fx_offset_ + i * 4;
    fx[0] = 128;
    fx[1] = 128;
    fx[2] = 128;
    fx[3] = 0;
  }
  std::memset(bytes + clear_chart_offset_, 0, texels);

  // Per-tile uv mapping, read by the forward pass to reproduce what the bake
  // did. Identity until a receiver claims the tile and says otherwise.
  for (u32 f = 0; f < Device::kMaxFramesInFlight; ++f) {
    tile_uv_xform_[f] =
        device.CreateBuffer(kMaxTiles * 4 * sizeof(f32), kBufferUsageStorage, true);
    if (!tile_uv_xform_[f].mapped) {
      RX_WARN("decal baker: tile transform buffer mapping failed; texture-space decals disabled");
      return false;
    }
    for (u32 i = 0; i < kMaxTiles; ++i) {
      f32* row = static_cast<f32*>(tile_uv_xform_[f].mapped) + i * 4;
      row[0] = 1;
      row[1] = 1;
      row[2] = 0;
      row[3] = 0;
    }
  }

  white_ = device.CreateImage2D(Format::kRGBA8Unorm, {1, 1},
                                kTextureUsageSampled | kTextureUsageTransferDst);
  flat_normal_ = device.CreateImage2D(Format::kRGBA8Unorm, {1, 1},
                                      kTextureUsageSampled | kTextureUsageTransferDst);
  if (!white_ || !flat_normal_) {
    RX_WARN("decal baker: fallback page allocation failed; texture-space decals disabled");
    return false;
  }
  const f32 opaque_white[4] = {1, 1, 1, 1};
  const f32 flat[4] = {0.5f, 0.5f, 1.0f, 1.0f};
  device.ImmediateSubmit([&](CommandList& cmd) {
    TextureBarrier to_copy[2] = {
        Transition(white_, ResourceState::kUndefined, ResourceState::kCopyDst),
        Transition(flat_normal_, ResourceState::kUndefined, ResourceState::kCopyDst)};
    cmd.TextureBarriers(to_copy);
    cmd.ClearColor(white_, opaque_white);
    cmd.ClearColor(flat_normal_, flat);
    TextureBarrier to_read[2] = {
        Transition(white_, ResourceState::kCopyDst, ResourceState::kShaderReadFragment),
        Transition(flat_normal_, ResourceState::kCopyDst, ResourceState::kShaderReadFragment)};
    cmd.TextureBarriers(to_read);
  });

  for (u32 f = 0; f < Device::kMaxFramesInFlight; ++f) {
    stamps_[f] = device.CreateBuffer(kMaxFrameStamps * sizeof(GpuStamp), kBufferUsageStorage, true);
    if (!stamps_[f].mapped) {
      RX_WARN("decal baker: stamp buffer mapping failed; texture-space decals disabled");
      return false;
    }
  }
  return true;
}

bool DecalBaker::CreatePipelines(Device& device) {
  sampler_ = device.GetSampler({.min_filter = Filter::kLinear,
                                .mag_filter = Filter::kLinear,
                                .address_u = AddressMode::kClampToEdge,
                                .address_v = AddressMode::kClampToEdge,
                                .address_w = AddressMode::kClampToEdge});

  VertexBufferLayout stream{
      .stride = sizeof(asset::Vertex),
      .attributes = {{0, Format::kRGB32Float, offsetof(asset::Vertex, position)},
                     {1, Format::kRGB32Float, offsetof(asset::Vertex, normal)},
                     {2, Format::kRGBA32Float, offsetof(asset::Vertex, tangent)},
                     {3, Format::kRG32Float, offsetof(asset::Vertex, uv)}}};
  VertexBufferLayout skin_stream{
      .stride = sizeof(asset::SkinnedVertexExtra),
      .attributes = {{5, Format::kRGBA8Uint, offsetof(asset::SkinnedVertexExtra, bone_indices)},
                     {6, Format::kRGBA8Unorm, offsetof(asset::SkinnedVertexExtra, bone_weights)}}};

  GraphicsPipelineDesc bake{};
  bake.vertex = RX_SHADER(k_decal_bake_vs_hlsl);
  bake.fragment = RX_SHADER(k_decal_bake_ps_hlsl);
  bake.vertex_buffers = {stream};
  // UV-space rasterization has no meaningful winding, and mirrored charts flip
  // it anyway; both faces must land in the tile.
  bake.raster = {.cull = CullMode::kNone};
  bake.color_formats = {Format::kRGBA8Unorm, Format::kRGBA8Unorm, Format::kR8Unorm};
  // The shader emits ONE premultiplied composite of its whole stamp run, so the
  // fixed function only has to put that over what the tile already holds. The
  // chart mask is a plain write.
  bake.blend = {BlendMode::kPremultiplied, BlendMode::kPremultiplied, BlendMode::kOpaque};
  bake.sets = {{.slots = {{0, BindingType::kStorageBuffer},
                          {1, BindingType::kStorageBuffer},
                          {2, BindingType::kCombinedTextureSampler},
                          {3, BindingType::kCombinedTextureSampler}}}};
  bake.push_constant_size = PushSize<BakePush>();
  bake.debug_name = "decal_bake";
  stamp_pipeline_ = device.CreateGraphicsPipeline(bake);
  if (!stamp_pipeline_) {
    RX_WARN("decal baker: bake pipeline creation failed; texture-space decals disabled");
    return false;
  }

  GraphicsPipelineDesc skinned = bake;
  skinned.vertex = RX_SHADER(k_decal_bake_skin_vs_hlsl);
  skinned.vertex_buffers = {stream, skin_stream};
  skinned.debug_name = "decal_bake_skinned";
  stamp_skin_pipeline_ = device.CreateGraphicsPipeline(skinned);
  if (!stamp_skin_pipeline_) {
    // Static receivers still work; skinned ones fall back to the bind pose,
    // which is wrong, so they are skipped instead.
    RX_WARN("decal baker: skinned bake pipeline unavailable; skinned receivers skipped");
  }

  dilate_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_decal_dilate_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kStorageImage},
                          {2, BindingType::kCombinedTextureSampler}}}},
      .push_constant_size = PushSize<DilatePush>(),
      .debug_name = "decal_dilate",
  });
  if (!dilate_pipeline_) {
    RX_WARN("decal baker: dilate pipeline creation failed; seams stay un-gutted");
  }
  return true;
}

void DecalBaker::Destroy(Device& device) {
  if (stamp_pipeline_) device.DestroyPipeline(stamp_pipeline_);
  if (stamp_skin_pipeline_) device.DestroyPipeline(stamp_skin_pipeline_);
  if (dilate_pipeline_) device.DestroyPipeline(dilate_pipeline_);
  stamp_pipeline_ = {};
  stamp_skin_pipeline_ = {};
  dilate_pipeline_ = {};
  if (albedo_mip0_) device.DestroyView(albedo_mip0_);
  if (fx_mip0_) device.DestroyView(fx_mip0_);
  albedo_mip0_ = {};
  fx_mip0_ = {};
  if (white_) device.DestroyImage(white_);
  if (flat_normal_) device.DestroyImage(flat_normal_);
  if (albedo_) device.DestroyImage(albedo_);
  if (fx_) device.DestroyImage(fx_);
  if (chart_) device.DestroyImage(chart_);
  if (clear_staging_) device.DestroyBuffer(clear_staging_);
  for (u32 f = 0; f < Device::kMaxFramesInFlight; ++f) {
    if (tile_uv_xform_[f]) device.DestroyBuffer(tile_uv_xform_[f]);
  }
  for (u32 f = 0; f < Device::kMaxFramesInFlight; ++f) {
    if (stamps_[f]) device.DestroyBuffer(stamps_[f]);
  }
  receivers_.clear();
  free_receivers_.clear();
  tile_owner_.clear();
  atlas_state_ = ResourceState::kUndefined;
  chart_state_ = ResourceState::kUndefined;
  stats_ = Stats{};
}

DecalBaker::Receiver* DecalBaker::find(u32 receiver) {
  if (receiver == 0 || receiver > receivers_.size()) return nullptr;
  Receiver& r = receivers_[receiver - 1];
  return r.alive ? &r : nullptr;
}

const DecalBaker::Receiver* DecalBaker::find(u32 receiver) const {
  return const_cast<DecalBaker*>(this)->find(receiver);
}

u32 DecalBaker::AcquireReceiver() {
  if (!available()) return 0;
  if (!free_receivers_.empty()) {
    const u32 handle = free_receivers_.back();
    free_receivers_.pop_back();
    Receiver& r = receivers_[handle - 1];
    r = Receiver{};
    r.alive = true;
    ++stats_.receivers;
    return handle;
  }
  Receiver fresh;
  fresh.alive = true;
  receivers_.push_back(std::move(fresh));
  ++stats_.receivers;
  return static_cast<u32>(receivers_.size());
}

void DecalBaker::ReleaseReceiver(u32 receiver) {
  Receiver* r = find(receiver);
  if (!r) return;
  if (r->tile != kNoTile) {
    tile_owner_[r->tile] = 0;
    --stats_.resident_tiles;
  }
  stats_.journalled -= static_cast<u32>(r->journal.size());
  *r = Receiver{};
  free_receivers_.push_back(receiver);
  --stats_.receivers;
}

bool DecalBaker::Stamp(const DecalStamp& stamp) {
  Receiver* r = find(stamp.receiver);
  if (!r) return false;
  GpuStamp gpu;
  std::memcpy(gpu.row0, stamp.projector.row0, sizeof(gpu.row0));
  std::memcpy(gpu.row1, stamp.projector.row1, sizeof(gpu.row1));
  std::memcpy(gpu.row2, stamp.projector.row2, sizeof(gpu.row2));
  std::memcpy(gpu.uv_rect, stamp.projector.uv_rect, sizeof(gpu.uv_rect));
  std::memcpy(gpu.tint_blend, stamp.projector.tint_blend, sizeof(gpu.tint_blend));
  std::memcpy(gpu.params2, stamp.projector.params2, sizeof(gpu.params2));

  r->journal.push_back(gpu);
  if (r->journal.size() > desc_.journal_limit) {
    r->journal.erase(r->journal.begin());
  } else {
    ++stats_.journalled;
  }
  // A tile carrying more pending work than a full replay costs may as well be
  // repainted from the journal, which also collapses the dropped stamps.
  r->pending.push_back(gpu);
  if (r->pending.size() >= desc_.journal_limit) {
    r->pending.clear();
    r->repaint = true;
  }
  return true;
}

void DecalBaker::SetReceiverUv(u32 receiver, f32 scale_u, f32 scale_v, f32 bias_u, f32 bias_v) {
  Receiver* r = find(receiver);
  if (!r) return;
  r->uv_scale[0] = scale_u;
  r->uv_scale[1] = scale_v;
  r->uv_bias[0] = bias_u;
  r->uv_bias[1] = bias_v;
  r->repaint = true;
}

void DecalBaker::ClearReceiver(u32 receiver) {
  Receiver* r = find(receiver);
  if (!r) return;
  stats_.journalled -= static_cast<u32>(r->journal.size());
  r->journal.clear();
  r->pending.clear();
  r->repaint = true;
}

u32 DecalBaker::tile_slot(u32 receiver) const {
  const Receiver* r = find(receiver);
  if (!r || r->tile == kNoTile) return 0;
  return r->tile + 1;
}

u32 DecalBaker::AcquireTile(u32 receiver, u64 frame_index) {
  for (u32 i = 0; i < tile_count_; ++i) {
    if (tile_owner_[i] == 0) {
      tile_owner_[i] = receiver;
      ++stats_.resident_tiles;
      return i;
    }
  }
  // Evict the coldest tile whose receiver is not drawing this frame; a receiver
  // touched this frame must keep what it has or the two would trade forever.
  u32 victim = kNoTile;
  u64 oldest = frame_index;
  for (u32 i = 0; i < tile_count_; ++i) {
    const Receiver* owner = find(tile_owner_[i]);
    if (!owner || owner->last_used >= frame_index) continue;
    if (victim == kNoTile || owner->last_used < oldest) {
      victim = i;
      oldest = owner->last_used;
    }
  }
  if (victim == kNoTile) return kNoTile;
  if (Receiver* owner = find(tile_owner_[victim])) {
    owner->tile = kNoTile;
    owner->pending.clear();
    // The journal is what makes eviction cheap: the decals come back on the
    // receiver's next draw, rebaked in one pass, with no memory held meanwhile.
    owner->repaint = true;
    ++stats_.evictions;
  }
  tile_owner_[victim] = receiver;
  return victim;
}

void DecalBaker::AddToGraph(RenderGraph& graph, std::span<const Target> targets, u32 frame_slot,
                            u64 frame_index, TextureView source_albedo, TextureView source_normal) {
  stats_.bakes = 0;
  if (!available()) return;

  // Mark every drawn receiver before anything can evict, so a tile in use this
  // frame is never the victim.
  for (const Target& target : targets) {
    if (Receiver* r = find(target.receiver)) r->last_used = frame_index;
  }

  struct BakeDraw {
    u32 tile = 0;
    const GpuMesh* mesh = nullptr;
    const GpuBuffer* bones = nullptr;
    Mat4 model = Mat4::Identity();
    u32 skin_offset = 0;
    u32 first_stamp = 0;
    u32 stamp_count = 0;
    bool clear = false;
    f32 uv_scale[2] = {1, 1};
    f32 uv_bias[2] = {0, 0};
  };
  base::Vector<BakeDraw> draws;
  GpuStamp* mapped = static_cast<GpuStamp*>(stamps_[frame_slot].mapped);
  u32 cursor = 0;

  // The forward pass reads the uv mapping per TILE, so every resident receiver
  // has to republish into this frame's slot: the buffer is per-frame-slot (the
  // previous frame may still be sampling the other one) and therefore stale.
  f32* xforms = static_cast<f32*>(tile_uv_xform_[frame_slot].mapped);
  for (const Receiver& receiver : receivers_) {
    if (!receiver.alive || receiver.tile == kNoTile) continue;
    f32* row = xforms + receiver.tile * 4;
    row[0] = receiver.uv_scale[0];
    row[1] = receiver.uv_scale[1];
    row[2] = receiver.uv_bias[0];
    row[3] = receiver.uv_bias[1];
  }

  for (const Target& target : targets) {
    Receiver* r = find(target.receiver);
    if (!r || !target.mesh || target.mesh->index_count == 0) continue;
    // A repaint only means something once there is a tile to repaint or a
    // history to replay. Without that guard, ClearReceiver / SetReceiverUv on an
    // un-stamped receiver would take (and evict for) a tile just to clear it.
    const bool holds_tile = r->tile != kNoTile;
    const bool wants =
        !r->pending.empty() || (r->repaint && (holds_tile || !r->journal.empty()));
    if (!wants) {
      r->repaint = r->repaint && holds_tile;
      continue;
    }

    const bool skinned = target.mesh->skinned && target.mesh->skinning && target.bones;
    if (target.mesh->skinned && (!skinned || !stamp_skin_pipeline_)) continue;

    // A tile the receiver does not hold yet always replays the journal. Size the
    // run and check the frame budget BEFORE acquiring: bailing afterwards would
    // leave the receiver owning a tile it never cleared, and the forward pass
    // would shade it with the evicted owner's decals.
    const bool repaint = r->repaint || !holds_tile;
    const u32 count = static_cast<u32>((repaint ? r->journal : r->pending).size());
    if (cursor + count > kMaxFrameStamps) continue;  // budget spent; next frame

    if (!holds_tile) {
      r->tile = AcquireTile(target.receiver, frame_index);
      if (r->tile == kNoTile) continue;  // every tile belongs to a live receiver
      r->repaint = true;
      f32* row = xforms + r->tile * 4;
      row[0] = r->uv_scale[0];
      row[1] = r->uv_scale[1];
      row[2] = r->uv_bias[0];
      row[3] = r->uv_bias[1];
    }
    const base::Vector<GpuStamp>& run = repaint ? r->journal : r->pending;

    BakeDraw draw;
    draw.tile = r->tile;
    draw.mesh = target.mesh;
    draw.bones = skinned ? target.bones : nullptr;
    draw.model = target.transform;
    draw.skin_offset = target.skin_offset;
    draw.first_stamp = cursor;
    draw.stamp_count = count;
    draw.clear = r->repaint;
    draw.uv_scale[0] = r->uv_scale[0];
    draw.uv_scale[1] = r->uv_scale[1];
    draw.uv_bias[0] = r->uv_bias[0];
    draw.uv_bias[1] = r->uv_bias[1];
    if (count > 0) {
      std::memcpy(mapped + cursor, run.data(), count * sizeof(GpuStamp));
      cursor += count;
    }
    draws.push_back(draw);

    r->pending.clear();
    r->repaint = false;
  }
  if (draws.empty()) return;
  stats_.bakes = static_cast<u32>(draws.size());

  graph.AddPass(
      "decal_bake", [](RenderGraph::PassBuilder&) {},
      [this, draws = std::move(draws), frame_slot, source_albedo,
       source_normal](PassContext& ctx) {
        CommandList& cmd = *ctx.cmd;
        ResourceState atlas = atlas_state_;
        ResourceState chart = chart_state_;

        // Repainted tiles get their neutral content back before anything draws
        // over them: this is also what wipes the previous owner of a recycled
        // tile, including its chart mask.
        bool any_clear = false;
        for (const BakeDraw& draw : draws) any_clear |= draw.clear;
        if (any_clear) {
          TextureBarrier to_copy[3] = {Transition(albedo_, atlas, ResourceState::kCopyDst),
                                       Transition(fx_, atlas, ResourceState::kCopyDst),
                                       Transition(chart_, chart, ResourceState::kCopyDst)};
          cmd.TextureBarriers(to_copy);
          atlas = ResourceState::kCopyDst;
          chart = ResourceState::kCopyDst;
          for (const BakeDraw& draw : draws) {
            if (!draw.clear) continue;
            const i32 x = static_cast<i32>((draw.tile % tiles_per_row_) * desc_.tile_size);
            const i32 y = static_cast<i32>((draw.tile / tiles_per_row_) * desc_.tile_size);
            BufferTextureCopy region{.offset = {x, y}, .extent = {desc_.tile_size, desc_.tile_size}};
            region.buffer_offset = clear_albedo_offset_;
            cmd.CopyBufferToTexture(clear_staging_, albedo_, {&region, 1});
            region.buffer_offset = clear_fx_offset_;
            cmd.CopyBufferToTexture(clear_staging_, fx_, {&region, 1});
            region.buffer_offset = clear_chart_offset_;
            cmd.CopyBufferToTexture(clear_staging_, chart_, {&region, 1});
          }
        }

        TextureBarrier to_target[3] = {
            Transition(albedo_, atlas, ResourceState::kColorTarget),
            Transition(fx_, atlas, ResourceState::kColorTarget),
            Transition(chart_, chart, ResourceState::kColorTarget)};
        cmd.TextureBarriers(to_target);

        ColorAttachment colors[3];
        colors[0] = {.view = albedo_mip0_, .load = LoadOp::kLoad};
        colors[1] = {.view = fx_mip0_, .load = LoadOp::kLoad};
        colors[2] = {.view = chart_.view, .load = LoadOp::kLoad};
        cmd.BeginRendering({.extent = {desc_.atlas_size, desc_.atlas_size},
                            .colors = {colors, 3}});
        PipelineHandle bound{};
        for (const BakeDraw& draw : draws) {
          if (draw.stamp_count == 0) continue;
          const PipelineHandle wanted = draw.bones ? stamp_skin_pipeline_ : stamp_pipeline_;
          if (!(wanted == bound)) {
            cmd.BindPipeline(wanted);
            bound = wanted;
          }
          // Transient sets are rewritten per draw: the palette differs between
          // receivers, and a static one binds the stamp buffer as a placeholder
          // for the slot its vertex shader never reads.
          cmd.BindTransient(
              0, {Bind::StorageBuffer(0, stamps_[frame_slot], 0, stamps_[frame_slot].size),
                  Bind::StorageBuffer(1, draw.bones ? *draw.bones : stamps_[frame_slot], 0,
                                      draw.bones ? draw.bones->size : stamps_[frame_slot].size),
                  Bind::Combined(2, source_albedo ? source_albedo : white_.view, sampler_),
                  Bind::Combined(3, source_normal ? source_normal : flat_normal_.view,
                                 sampler_)});
          const f32 x = static_cast<f32>((draw.tile % tiles_per_row_) * desc_.tile_size);
          const f32 y = static_cast<f32>((draw.tile / tiles_per_row_) * desc_.tile_size);
          cmd.SetViewport(x, y, static_cast<f32>(desc_.tile_size),
                          static_cast<f32>(desc_.tile_size));
          cmd.SetScissor(static_cast<i32>(x), static_cast<i32>(y), desc_.tile_size,
                         desc_.tile_size);
          BakePush push;
          push.model = draw.model;
          push.first_stamp = draw.first_stamp;
          push.stamp_count = draw.stamp_count;
          push.skin_offset = draw.skin_offset;
          push.uv_scale[0] = draw.uv_scale[0];
          push.uv_scale[1] = draw.uv_scale[1];
          push.uv_bias[0] = draw.uv_bias[0];
          push.uv_bias[1] = draw.uv_bias[1];
          cmd.Push(push);
          cmd.BindVertexBuffer(0, draw.mesh->vertices);
          if (draw.bones) cmd.BindVertexBuffer(1, draw.mesh->skinning);
          cmd.BindIndexBuffer(draw.mesh->indices, 0, IndexType::kUint32);
          cmd.DrawIndexed(draw.mesh->index_count);
        }
        cmd.EndRendering();

        // Gutter fill, then the mip chain the forward pass minifies through.
        if (dilate_pipeline_) {
          TextureBarrier to_storage[3] = {
              Transition(albedo_, ResourceState::kColorTarget, ResourceState::kGeneral),
              Transition(fx_, ResourceState::kColorTarget, ResourceState::kGeneral),
              Transition(chart_, ResourceState::kColorTarget,
                         ResourceState::kShaderReadCompute)};
          to_storage[0].mip_count = 1;
          to_storage[1].mip_count = 1;
          cmd.TextureBarriers(to_storage);
          cmd.BindPipeline(dilate_pipeline_);
          for (const BakeDraw& draw : draws) {
            if (draw.stamp_count == 0) continue;
            cmd.BindTransient(0, {Bind::StorageView(0, albedo_mip0_),
                                  Bind::StorageView(1, fx_mip0_),
                                  Bind::Combined(2, chart_.view, sampler_)});
            DilatePush push{{(draw.tile % tiles_per_row_) * desc_.tile_size,
                             (draw.tile / tiles_per_row_) * desc_.tile_size},
                            desc_.tile_size, 0};
            cmd.Push(push);
            cmd.Dispatch2D({desc_.tile_size, desc_.tile_size});
          }
        }

        // Mip chain: blitting the whole atlas is simpler than tracking dirty
        // regions and costs a third of a mip-0 copy. A mip texel never spans
        // two tiles (the chain stops far short of a tile edge), so tiles cannot
        // bleed into each other.
        const ResourceState after_dilate =
            dilate_pipeline_ ? ResourceState::kGeneral : ResourceState::kColorTarget;
        if (mip_count_ > 1) {
          TextureBarrier chain[4] = {
              Transition(albedo_, after_dilate, ResourceState::kCopySrc),
              Transition(fx_, after_dilate, ResourceState::kCopySrc),
              Transition(albedo_, ResourceState::kUndefined, ResourceState::kCopyDst),
              Transition(fx_, ResourceState::kUndefined, ResourceState::kCopyDst)};
          chain[0].mip_count = 1;
          chain[1].mip_count = 1;
          chain[2].base_mip = 1;
          chain[3].base_mip = 1;
          cmd.TextureBarriers(chain);
          u32 size = desc_.atlas_size;
          for (u32 mip = 1; mip < mip_count_; ++mip) {
            const Extent2D src{size, size};
            const Extent2D dst{std::max(size >> 1, 1u), std::max(size >> 1, 1u)};
            cmd.BlitMip(albedo_, mip - 1, src, mip, dst);
            cmd.BlitMip(fx_, mip - 1, src, mip, dst);
            if (mip + 1 < mip_count_) {
              TextureBarrier step[2] = {Transition(albedo_, ResourceState::kCopyDst,
                                                   ResourceState::kCopySrc),
                                        Transition(fx_, ResourceState::kCopyDst,
                                                   ResourceState::kCopySrc)};
              step[0].base_mip = mip;
              step[0].mip_count = 1;
              step[1].base_mip = mip;
              step[1].mip_count = 1;
              cmd.TextureBarriers(step);
            }
            size = dst.width;
          }
          TextureBarrier to_read[4] = {
              Transition(albedo_, ResourceState::kCopySrc, ResourceState::kShaderReadFragment),
              Transition(fx_, ResourceState::kCopySrc, ResourceState::kShaderReadFragment),
              Transition(albedo_, ResourceState::kCopyDst, ResourceState::kShaderReadFragment),
              Transition(fx_, ResourceState::kCopyDst, ResourceState::kShaderReadFragment)};
          to_read[0].mip_count = mip_count_ - 1;
          to_read[1].mip_count = mip_count_ - 1;
          to_read[2].base_mip = mip_count_ - 1;
          to_read[3].base_mip = mip_count_ - 1;
          cmd.TextureBarriers(to_read);
        } else {
          TextureBarrier to_read[2] = {
              Transition(albedo_, after_dilate, ResourceState::kShaderReadFragment),
              Transition(fx_, after_dilate, ResourceState::kShaderReadFragment)};
          cmd.TextureBarriers(to_read);
        }
        atlas_state_ = ResourceState::kShaderReadFragment;
        chart_state_ =
            dilate_pipeline_ ? ResourceState::kShaderReadCompute : ResourceState::kColorTarget;
      });
}

}  // namespace rx::render
