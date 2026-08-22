#include "render/geometry/imposters.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "shaders/imposter_bake_ps_hlsl.h"
#include "shaders/imposter_bake_vs_hlsl.h"
#include "shaders/imposter_ps_hlsl.h"
#include "shaders/imposter_vs_hlsl.h"

namespace rx::render {
namespace {

// A few mips kill the minification moire on far billboards (the stacked
// cone edges alias hard at 16:1 otherwise); deeper mips would bleed
// neighboring atlas cells together.
constexpr u32 kAtlasMips = 4;

struct BakePush {
  Mat4 view_proj;
  f32 alpha_cutoff;  // 0 = no cutout
  f32 textured;      // 0 = albedo from the vertex colour
  f32 pad[2];
};

struct DrawPush {
  Mat4 view_proj;
  f32 camera[4];
  f32 sun[4];
  f32 sun_color[4];
  f32 grid;        // octahedral cells per axis within one mesh's tile
  f32 tile_scale;  // 1 / kMeshGrid: a tile's share of the atlas
  f32 pad0;
  f32 pad1;
};

ByteSpan Span(const void* data, size_t bytes) {
  return ByteSpan(static_cast<const u8*>(data), bytes);
}

// Inverse of the shader's HemiOctEncode: cell-center uv -> hemisphere dir.
Vec3 HemiOctDecode(f32 u, f32 v) {
  f32 ex = u * 2.0f - 1.0f;
  f32 ey = v * 2.0f - 1.0f;
  Vec3 d;
  d.x = (ex - ey) * 0.5f;
  d.z = (ex + ey) * 0.5f;
  d.y = 1.0f - std::abs(d.x) - std::abs(d.z);
  return Normalize(d);
}

}  // namespace

bool ImposterPass::Initialize(Device& device, Format color_format, Format depth_format) {
  bake_pipeline_ = device.CreateGraphicsPipeline({
      .vertex = RX_SHADER(k_imposter_bake_vs_hlsl),
      .fragment = RX_SHADER(k_imposter_bake_ps_hlsl),
      .vertex_buffers = {{.stride = sizeof(asset::Vertex),
                          .attributes = {{0, Format::kRGB32Float, 0},
                                         {1, Format::kRGB32Float, 12},
                                         {2, Format::kRGBA32Float, 24},
                                         {3, Format::kRG32Float, 40},
                                         {4, Format::kRGBA8Unorm, 48}}}},
      .raster = {.cull = CullMode::kNone},  // thin foliage bakes double-sided
      // Orthographic() is forward-z (near = 0): kLess + clear 1, like the csm.
      .depth = {.test = true, .write = true, .compare = CompareOp::kLess,
                .format = Format::kD32Float},
      .color_formats = {Format::kRGBA8Unorm, Format::kRGBA8Unorm},
      .blend = {BlendMode::kOpaque, BlendMode::kOpaque},
      .sets = {{.slots = {{0, BindingType::kCombinedTextureSampler}}}},
      .push_constant_size = PushSize<BakePush>(),
      .debug_name = "imposter_bake",
  });
  draw_pipeline_ = device.CreateGraphicsPipeline({
      .vertex = RX_SHADER(k_imposter_vs_hlsl),
      .fragment = RX_SHADER(k_imposter_ps_hlsl),
      .topology = PrimitiveTopology::kTriangleStrip,
      .raster = {.cull = CullMode::kNone},
      .depth = {.test = true, .write = true, .compare = CompareOp::kGreaterEqual,
                .format = depth_format},
      .color_formats = {color_format},
      .sets = {{.slots = {{0, BindingType::kStorageBuffer},
                          {1, BindingType::kCombinedTextureSampler},
                          {2, BindingType::kCombinedTextureSampler},
                          {3, BindingType::kStorageBuffer}}}},
      .push_constant_size = PushSize<DrawPush>(),
      .debug_name = "imposter_draw",
  });
  if (!bake_pipeline_ || !draw_pipeline_) {
    RX_ERROR("imposter pipeline creation failed");
    return false;
  }
  sampler_ = device.GetSampler({.min_filter = Filter::kLinear,
                                .mag_filter = Filter::kLinear,
                                .mip_filter = Filter::kLinear,
                                .address_u = AddressMode::kClampToEdge,
                                .address_v = AddressMode::kClampToEdge});

  // The bake pipeline always samples a base-colour map, so a submesh that has
  // none needs something bound: white leaves the vertex-colour branch alone.
  white_ = device.CreateImage2D(Format::kRGBA8Unorm, {1, 1},
                                kTextureUsageSampled | kTextureUsageTransferDst);
  if (!white_) return false;
  const u8 pixel[4] = {255, 255, 255, 255};
  GpuBuffer staging = device.CreateBufferWithData(Span(pixel, sizeof(pixel)),
                                                  kBufferUsageTransferSrc);
  device.ImmediateSubmit([&](CommandList& cmd) {
    TextureBarrier to_dst[1] = {
        Transition(white_, ResourceState::kUndefined, ResourceState::kCopyDst)};
    cmd.TextureBarriers(to_dst);
    BufferTextureCopy copy;
    copy.extent = {1, 1};
    cmd.CopyBufferToTexture(staging, white_, {&copy, 1});
    TextureBarrier to_read[1] = {Transition(white_, ResourceState::kCopyDst,
                                            ResourceState::kShaderReadFragment)};
    cmd.TextureBarriers(to_read);
  });
  device.DestroyBuffer(staging);
  return true;
}

void ImposterPass::Destroy(Device& device) {
  for (PipelineHandle* p : {&bake_pipeline_, &draw_pipeline_}) {
    if (*p) device.DestroyPipeline(*p);
    *p = {};
  }
  for (GpuImage* img : {&albedo_atlas_, &normal_atlas_, &white_}) {
    if (*img) device.DestroyImage(*img);
    *img = {};
  }
  for (GpuBuffer* buf : {&instances_, &mesh_params_}) {
    if (*buf) device.DestroyBuffer(*buf);
    *buf = {};
  }
  mesh_count_ = 0;
  instance_count_ = 0;
}

u32 ImposterPass::Bake(Device& device, const asset::Mesh& mesh,
                       std::span<const BakeMaterial> materials) {
  if (!bake_pipeline_ || mesh.lods.empty()) return kNoMesh;
  if (mesh_count_ >= kMaxMeshes) {
    RX_WARN("imposter atlas is full at {} meshes; this one is not baked", kMaxMeshes);
    return kNoMesh;
  }
  const asset::MeshLod& lod = mesh.lods[0];
  if (lod.indices.empty()) return kNoMesh;
  const u32 slot = mesh_count_;
  const bool first = slot == 0;

  // Bounds of the mesh (bake frames fit this sphere).
  Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
  for (const asset::Vertex& v : lod.vertices) {
    lo = {std::min(lo.x, v.position[0]), std::min(lo.y, v.position[1]),
          std::min(lo.z, v.position[2])};
    hi = {std::max(hi.x, v.position[0]), std::max(hi.y, v.position[1]),
          std::max(hi.z, v.position[2])};
  }
  Vec3 center = (lo + hi) * 0.5f;
  Vec3 ext = (hi - lo) * 0.5f;
  const f32 radius = std::sqrt(ext.x * ext.x + ext.y * ext.y + ext.z * ext.z);

  if (first) {
    albedo_atlas_ = device.CreateImage2D(
        Format::kRGBA8Unorm, {kAtlas, kAtlas},
        kTextureUsageSampled | kTextureUsageColorTarget | kTextureUsageTransferSrc |
            kTextureUsageTransferDst,
        kAtlasMips);
    normal_atlas_ = device.CreateImage2D(
        Format::kRGBA8Unorm, {kAtlas, kAtlas},
        kTextureUsageSampled | kTextureUsageColorTarget | kTextureUsageTransferSrc |
            kTextureUsageTransferDst,
        kAtlasMips);
  }
  if (!albedo_atlas_ || !normal_atlas_) return kNoMesh;
  // Only live for this bake: a full-atlas depth buffer is 16 MB and nothing
  // after the last bake reads it.
  GpuImage bake_depth = device.CreateImage2D(Format::kD32Float, {kAtlas, kAtlas},
                                             kTextureUsageDepthTarget);
  if (!bake_depth) return kNoMesh;

  GpuBuffer vertices = device.CreateBufferWithData(
      Span(lod.vertices.data(), lod.vertices.size() * sizeof(asset::Vertex)),
      kBufferUsageVertex);
  GpuBuffer indices = device.CreateBufferWithData(
      Span(lod.indices.data(), lod.indices.size() * sizeof(u32)), kBufferUsageIndex);

  const u32 tile_x = (slot % kMeshGrid) * kTile;
  const u32 tile_y = (slot / kMeshGrid) * kTile;

  device.ImmediateSubmit([&](CommandList& cmd) {
    // The first bake clears the whole atlas, so every later bake loads: the
    // tiles already baked have to survive, and an untouched tile keeps the
    // alpha 0 that first clear left.
    const ResourceState before =
        first ? ResourceState::kUndefined : ResourceState::kShaderReadFragment;
    TextureBarrier to_target[2] = {
        Transition(albedo_atlas_, before, ResourceState::kColorTarget),
        Transition(normal_atlas_, before, ResourceState::kColorTarget)};
    cmd.TextureBarriers(to_target);
    TextureBarrier depth_target[1] = {
        Transition(bake_depth, ResourceState::kUndefined, ResourceState::kDepthTarget)};
    cmd.TextureBarriers(depth_target);

    const LoadOp load = first ? LoadOp::kClear : LoadOp::kLoad;
    ColorAttachment colors[2];
    colors[0] = {.view = albedo_atlas_.view, .load = load, .clear = {0, 0, 0, 0}};
    colors[1] = {.view = normal_atlas_.view, .load = load, .clear = {0.5f, 1, 0.5f, 0}};
    DepthAttachment depth{.view = bake_depth.view, .load = LoadOp::kClear, .clear = 1.0f};
    cmd.BeginRendering({.extent = {kAtlas, kAtlas}, .colors = {colors, 2}, .depth = &depth});
    cmd.BindPipeline(bake_pipeline_);
    cmd.BindVertexBuffer(0, vertices, 0);
    cmd.BindIndexBuffer(indices, 0, IndexType::kUint32);
    for (u32 j = 0; j < kGrid; ++j) {
      for (u32 i = 0; i < kGrid; ++i) {
        Vec3 dir = HemiOctDecode((i + 0.5f) / kGrid, (j + 0.5f) / kGrid);
        Vec3 up = std::abs(dir.y) > 0.98f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
        Mat4 view = LookAt(center + dir * (radius * 2.0f), center, up);
        Mat4 proj = Orthographic(-radius, radius, -radius, radius, 0.1f, radius * 4.0f);
        const f32 x = static_cast<f32>(tile_x + i * kCell);
        const f32 y = static_cast<f32>(tile_y + j * kCell);
        cmd.SetViewport(x, y, static_cast<f32>(kCell), static_cast<f32>(kCell));
        cmd.SetScissor(static_cast<i32>(x), static_cast<i32>(y), kCell, kCell);
        // One draw per submesh, because the albedo map and the cutoff are the
        // material's: a tree is bark plus an alpha-masked foliage sheet, and
        // baking the two through one state loses the leaf shape entirely.
        const u32 draws = std::max<u32>(1, static_cast<u32>(lod.submeshes.size()));
        for (u32 s = 0; s < draws; ++s) {
          const BakeMaterial material = s < materials.size() ? materials[s] : BakeMaterial{};
          const GpuImage& albedo = material.base_color ? *material.base_color : white_;
          BakePush push{};
          push.view_proj = proj * view;
          push.alpha_cutoff = material.alpha_cutoff;
          push.textured = material.base_color ? 1.0f : 0.0f;
          cmd.BindTransient(0, {Bind::Combined(0, albedo.view, sampler_)});
          cmd.Push(push);
          // A mesh with no submeshes is one implicit range over every index.
          if (lod.submeshes.empty()) {
            cmd.DrawIndexed(static_cast<u32>(lod.indices.size()), 1, 0, 0, 0);
          } else {
            cmd.DrawIndexed(lod.submeshes[s].index_count, 1, lod.submeshes[s].index_offset, 0, 0);
          }
        }
      }
    }
    cmd.EndRendering();

    // Mip chain: blit each level down, then settle everything shader-read.
    for (GpuImage* atlas : {&albedo_atlas_, &normal_atlas_}) {
      TextureBarrier to_src[1] = {{.texture = atlas->handle,
                                   .before = ResourceState::kColorTarget,
                                   .after = ResourceState::kCopySrc,
                                   .base_mip = 0,
                                   .mip_count = 1}};
      cmd.TextureBarriers(to_src);
      for (u32 mip = 1; mip < kAtlasMips; ++mip) {
        TextureBarrier to_dst[1] = {{.texture = atlas->handle,
                                     .before = ResourceState::kUndefined,
                                     .after = ResourceState::kCopyDst,
                                     .base_mip = mip,
                                     .mip_count = 1}};
        cmd.TextureBarriers(to_dst);
        cmd.BlitMip(*atlas, mip - 1, {kAtlas >> (mip - 1), kAtlas >> (mip - 1)}, mip,
                    {kAtlas >> mip, kAtlas >> mip});
        TextureBarrier next_src[1] = {{.texture = atlas->handle,
                                       .before = ResourceState::kCopyDst,
                                       .after = ResourceState::kCopySrc,
                                       .base_mip = mip,
                                       .mip_count = 1}};
        cmd.TextureBarriers(next_src);
      }
      TextureBarrier to_read[1] = {Transition(*atlas, ResourceState::kCopySrc,
                                              ResourceState::kShaderReadFragment)};
      cmd.TextureBarriers(to_read);
    }
  });
  device.DestroyBuffer(vertices);
  device.DestroyBuffer(indices);
  device.DestroyImage(bake_depth);

  meshes_[slot].radius = radius;
  meshes_[slot].center_y = center.y;
  meshes_[slot].tile[0] = static_cast<f32>(tile_x) / static_cast<f32>(kAtlas);
  meshes_[slot].tile[1] = static_cast<f32>(tile_y) / static_cast<f32>(kAtlas);
  mesh_count_ = slot + 1;
  UploadMeshParams(device);

  // RX_IMPOSTER_DUMP=<path.ppm> writes the baked albedo atlas for inspection.
  if (const char* dump = std::getenv("RX_IMPOSTER_DUMP")) {
    GpuBuffer readback = device.CreateBuffer(static_cast<u64>(kAtlas) * kAtlas * 4,
                                             kBufferUsageTransferDst, true);
    device.ImmediateSubmit([&](CommandList& cmd) {
      TextureBarrier to_src[1] = {{.texture = albedo_atlas_.handle,
                                   .before = ResourceState::kShaderReadFragment,
                                   .after = ResourceState::kCopySrc,
                                   .base_mip = 0,
                                   .mip_count = 1}};
      cmd.TextureBarriers(to_src);
      BufferTextureCopy copy;
      copy.extent = {kAtlas, kAtlas};
      cmd.CopyTextureToBuffer(albedo_atlas_, readback, copy);
      TextureBarrier back[1] = {{.texture = albedo_atlas_.handle,
                                 .before = ResourceState::kCopySrc,
                                 .after = ResourceState::kShaderReadFragment,
                                 .base_mip = 0,
                                 .mip_count = 1}};
      cmd.TextureBarriers(back);
    });
    if (readback.mapped) {
      std::FILE* f = std::fopen(dump, "wb");
      if (f) {
        std::fprintf(f, "P6\n%u %u\n255\n", kAtlas, kAtlas);
        const u8* px = static_cast<const u8*>(readback.mapped);
        for (u32 i = 0; i < kAtlas * kAtlas; ++i) std::fwrite(px + i * 4, 1, 3, f);
        std::fclose(f);
        RX_INFO("imposter atlas dumped to {}", dump);
      }
    }
    device.DestroyBuffer(readback);
  }
  RX_INFO("imposter bake: slot {} of {}, {} views of {} tris (radius {:.2f})", slot,
          kMaxMeshes, kGrid * kGrid, lod.indices.size() / 3, radius);
  return slot;
}

void ImposterPass::UploadMeshParams(Device& device) {
  if (mesh_params_) device.DestroyBufferDeferred(mesh_params_);
  mesh_params_ = device.CreateBufferWithData(
      Span(meshes_, mesh_count_ * sizeof(MeshParams)), kBufferUsageStorage);
}

void ImposterPass::SetInstances(Device& device, std::span<const Instance> instances) {
  // Deferred: the split is rebuilt as the camera moves, so the buffer this
  // replaces may still be read by a submitted frame.
  if (instances_) device.DestroyBufferDeferred(instances_);
  instances_ = {};
  instance_count_ = static_cast<u32>(instances.size());
  if (instance_count_ == 0) return;
  instances_ = device.CreateBufferWithData(
      Span(instances.data(), instances.size() * sizeof(Instance)), kBufferUsageStorage);
}

void ImposterPass::AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                              Extent2D extent, const Frame& frame) {
  if (!active() || !instances_ || !mesh_params_) return;
  graph.AddPass(
      "imposters",
      [&](RenderGraph::PassBuilder& b) {
        b.Write(color, ResourceUsage::kColorAttachment);
        b.Write(depth, ResourceUsage::kDepthAttachment);
      },
      [this, color, depth, extent, frame](PassContext& ctx) {
        DrawPush push{};
        push.view_proj = frame.view_proj;
        push.camera[0] = frame.camera_pos.x;
        push.camera[1] = frame.camera_pos.y;
        push.camera[2] = frame.camera_pos.z;
        Vec3 sun = Normalize(frame.sun_direction);
        push.sun[0] = sun.x;
        push.sun[1] = sun.y;
        push.sun[2] = sun.z;
        push.sun[3] = frame.sun_intensity;
        push.sun_color[0] = frame.sun_color.x;
        push.sun_color[1] = frame.sun_color.y;
        push.sun_color[2] = frame.sun_color.z;
        push.sun_color[3] = frame.ambient;
        push.grid = static_cast<f32>(kGrid);
        push.tile_scale = 1.0f / static_cast<f32>(kMeshGrid);

        ColorAttachment att{.view = ctx.graph->image(color).view, .load = LoadOp::kLoad};
        DepthAttachment depth_att{.view = ctx.graph->image(depth).view, .load = LoadOp::kLoad};
        ctx.cmd->BeginRendering({.extent = extent, .colors = {&att, 1}, .depth = &depth_att});
        ctx.cmd->BindPipeline(draw_pipeline_);
        ctx.cmd->BindTransient(0, {Bind::StorageBuffer(0, instances_, 0, instances_.size),
                                   Bind::Combined(1, albedo_atlas_.view, sampler_),
                                   Bind::Combined(2, normal_atlas_.view, sampler_),
                                   Bind::StorageBuffer(3, mesh_params_, 0, mesh_params_.size)});
        ctx.cmd->Push(push);
        ctx.cmd->Draw(4, instance_count_, 0, 0);
        ctx.cmd->EndRendering();
      });
}

}  // namespace rx::render
