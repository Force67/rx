#include "render/geometry/imposters.h"

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

struct BakePush {
  Mat4 view_proj;
};

struct DrawPush {
  Mat4 view_proj;
  f32 camera[4];
  f32 sun[4];
  f32 sun_color[4];
  f32 radius;
  f32 center_y;
  f32 grid;
  f32 pad0;
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
      .push_constant_size = sizeof(BakePush),
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
                          {2, BindingType::kCombinedTextureSampler}}}},
      .push_constant_size = sizeof(DrawPush),
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
  return true;
}

void ImposterPass::Destroy(Device& device) {
  for (PipelineHandle* p : {&bake_pipeline_, &draw_pipeline_}) {
    if (*p) device.DestroyPipeline(*p);
    *p = {};
  }
  for (GpuImage* img : {&albedo_atlas_, &normal_atlas_, &bake_depth_}) {
    if (*img) device.DestroyImage(*img);
    *img = {};
  }
  if (instances_) device.DestroyBuffer(instances_);
  instances_ = {};
}

void ImposterPass::Bake(Device& device, const asset::Mesh& mesh) {
  if (!bake_pipeline_ || mesh.lods.empty()) return;
  const asset::MeshLod& lod = mesh.lods[0];

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
  mesh_radius_ = std::sqrt(ext.x * ext.x + ext.y * ext.y + ext.z * ext.z);
  mesh_center_y_ = center.y;

  // A few mips kill the minification moire on far billboards (the stacked
  // cone edges alias hard at 16:1 otherwise); deeper mips would bleed
  // neighboring atlas cells together.
  constexpr u32 kAtlasMips = 4;
  if (!albedo_atlas_) {
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
    bake_depth_ = device.CreateImage2D(Format::kD32Float, {kAtlas, kAtlas},
                                       kTextureUsageDepthTarget);
  }
  if (!albedo_atlas_ || !normal_atlas_ || !bake_depth_) return;

  GpuBuffer vertices = device.CreateBufferWithData(
      Span(lod.vertices.data(), lod.vertices.size() * sizeof(asset::Vertex)),
      kBufferUsageVertex);
  GpuBuffer indices = device.CreateBufferWithData(
      Span(lod.indices.data(), lod.indices.size() * sizeof(u32)), kBufferUsageIndex);

  device.ImmediateSubmit([&](CommandList& cmd) {
    TextureBarrier to_target[2] = {
        Transition(albedo_atlas_, ResourceState::kUndefined, ResourceState::kColorTarget),
        Transition(normal_atlas_, ResourceState::kUndefined, ResourceState::kColorTarget)};
    cmd.TextureBarriers(to_target);
    TextureBarrier depth_target[1] = {
        Transition(bake_depth_, ResourceState::kUndefined, ResourceState::kDepthTarget)};
    cmd.TextureBarriers(depth_target);

    ColorAttachment colors[2];
    colors[0] = {.view = albedo_atlas_.view, .load = LoadOp::kClear, .clear = {0, 0, 0, 0}};
    colors[1] = {.view = normal_atlas_.view, .load = LoadOp::kClear, .clear = {0.5f, 1, 0.5f, 0}};
    DepthAttachment depth{.view = bake_depth_.view, .load = LoadOp::kClear, .clear = 1.0f};
    cmd.BeginRendering({.extent = {kAtlas, kAtlas}, .colors = {colors, 2}, .depth = &depth});
    cmd.BindPipeline(bake_pipeline_);
    cmd.BindVertexBuffer(0, vertices, 0);
    cmd.BindIndexBuffer(indices, 0, IndexType::kUint32);
    for (u32 j = 0; j < kGrid; ++j) {
      for (u32 i = 0; i < kGrid; ++i) {
        Vec3 dir = HemiOctDecode((i + 0.5f) / kGrid, (j + 0.5f) / kGrid);
        Vec3 up = std::abs(dir.y) > 0.98f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
        Mat4 view = LookAt(center + dir * (mesh_radius_ * 2.0f), center, up);
        Mat4 proj = Orthographic(-mesh_radius_, mesh_radius_, -mesh_radius_, mesh_radius_,
                                 0.1f, mesh_radius_ * 4.0f);
        BakePush push{proj * view};
        cmd.SetViewport(static_cast<f32>(i * kCell), static_cast<f32>(j * kCell),
                        static_cast<f32>(kCell), static_cast<f32>(kCell));
        cmd.SetScissor(static_cast<i32>(i * kCell), static_cast<i32>(j * kCell), kCell, kCell);
        cmd.Push(push);
        cmd.DrawIndexed(static_cast<u32>(lod.indices.size()), 1, 0, 0, 0);
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
  baked_ = true;

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
  RX_INFO("imposter bake: {} views of {} tris (radius {:.2f})", kGrid * kGrid,
           lod.indices.size() / 3, mesh_radius_);
}

void ImposterPass::SetInstances(Device& device, std::span<const Instance> instances) {
  if (instances_) device.DestroyBuffer(instances_);
  instance_count_ = static_cast<u32>(instances.size());
  if (instance_count_ == 0) return;
  instances_ = device.CreateBufferWithData(
      Span(instances.data(), instances.size() * sizeof(Instance)), kBufferUsageStorage);
}

void ImposterPass::AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                              Extent2D extent, const Frame& frame) {
  if (!active()) return;
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
        push.radius = mesh_radius_;
        push.center_y = mesh_center_y_;
        push.grid = static_cast<f32>(kGrid);

        ColorAttachment att{.view = ctx.graph->image(color).view, .load = LoadOp::kLoad};
        DepthAttachment depth_att{.view = ctx.graph->image(depth).view, .load = LoadOp::kLoad};
        ctx.cmd->BeginRendering({.extent = extent, .colors = {&att, 1}, .depth = &depth_att});
        ctx.cmd->BindPipeline(draw_pipeline_);
        ctx.cmd->BindTransient(0, {Bind::StorageBuffer(0, instances_, 0, instances_.size),
                                   Bind::Combined(1, albedo_atlas_.view, sampler_),
                                   Bind::Combined(2, normal_atlas_.view, sampler_)});
        ctx.cmd->Push(push);
        ctx.cmd->Draw(4, instance_count_, 0, 0);
        ctx.cmd->EndRendering();
      });
}

}  // namespace rx::render
