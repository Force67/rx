#include "render2d/sprite_renderer.h"

#include <algorithm>
#include <cstring>

#include "core/log.h"
#include "render/rhi/bindings.h"

#include "shaders/sprite_vs_hlsl.h"
#include "shaders/sprite_ps_hlsl.h"
#include "shaders/light2d_vs_hlsl.h"
#include "shaders/light2d_ps_hlsl.h"
#include "shaders/composite_vs_hlsl.h"
#include "shaders/composite_ps_hlsl.h"

namespace rx::render2d {

using namespace rx::render;

static_assert(sizeof(GpuSprite) == 64, "GpuSprite must match the sprite.vs std430 layout");
static_assert(sizeof(GpuLight) == 48, "GpuLight must match the light2d.vs std430 layout");

namespace {
constexpr Format kTargetFormat = Format::kRGBA16Float;

ShaderBlob Spirv(const unsigned char* data, size_t size) {
  return ShaderBlob{data, size, ShaderFormat::kSpirv};
}
}  // namespace

bool SpriteRenderer::Init(render::Renderer& renderer) {
  renderer_ = &renderer;
  device_ = renderer.device();
  if (!device_ || device_->is_stub()) {
    RX_WARN("render2d: no device (headless/stub); staying inert");
    return false;
  }
  // The 2D content composites through the scene-opaque hook, which fires on the
  // Vulkan backend only.
  if (device_->caps().backend != Backend::kVulkan) {
    RX_WARN("render2d: not on the vulkan backend; staying inert");
    return false;
  }

  SamplerDesc sd;
  sd.min_filter = sd.mag_filter = sd.mip_filter = Filter::kLinear;
  sd.address_u = sd.address_v = sd.address_w = AddressMode::kClampToEdge;
  sampler_ = device_->GetSampler(sd);

  if (!CreatePipelines()) {
    RX_ERROR("render2d: pipeline creation failed");
    return false;
  }

  // textures_[0] unused, [1] = 1x1 white so solid quads need no art.
  textures_.push_back({});  // slot 0
  const u8 white[4] = {255, 255, 255, 255};
  TextureId w = CreateTexture(1, 1, white, "render2d_white");
  if (w != kWhiteTexture) {
    RX_ERROR("render2d: unexpected white texture id {}", w);
    return false;
  }

  ready_ = true;
  RX_INFO("render2d ready (sprite/tilemap/2d-lighting, backend={})",
          BackendName(device_->caps().backend));
  return true;
}

bool SpriteRenderer::CreatePipelines() {
  // Sprite pipeline: instanced quads pulled from a StructuredBuffer, alpha
  // blended into an RGBA16F target, no depth (painter's order on the CPU).
  {
    base::Vector<PipelineBindings> sets;
    sets.push_back({.slots = {{0, BindingType::kStorageBuffer},
                              {1, BindingType::kCombinedTextureSampler}}});
    GraphicsPipelineDesc desc{
        .vertex = Spirv(k_sprite_vs_hlsl, sizeof(k_sprite_vs_hlsl)),
        .fragment = Spirv(k_sprite_ps_hlsl, sizeof(k_sprite_ps_hlsl)),
        .topology = PrimitiveTopology::kTriangleList,
        .raster = {.cull = CullMode::kNone},
        .depth = {},
        .color_formats = {kTargetFormat},
        .blend = {BlendMode::kAlpha},
        .sets = sets,
        .push_constant_size = sizeof(Mat4),
        .debug_name = "render2d_sprite",
    };
    sprite_pipeline_ = device_->CreateGraphicsPipeline(desc);
  }

  // Light pipeline: one additive quad per light.
  {
    base::Vector<PipelineBindings> sets;
    sets.push_back({.slots = {{0, BindingType::kStorageBuffer}}});
    GraphicsPipelineDesc desc{
        .vertex = Spirv(k_light2d_vs_hlsl, sizeof(k_light2d_vs_hlsl)),
        .fragment = Spirv(k_light2d_ps_hlsl, sizeof(k_light2d_ps_hlsl)),
        .topology = PrimitiveTopology::kTriangleList,
        .raster = {.cull = CullMode::kNone},
        .depth = {},
        .color_formats = {kTargetFormat},
        .blend = {BlendMode::kAdditive},
        .sets = sets,
        .push_constant_size = sizeof(Mat4),
        .debug_name = "render2d_light",
    };
    light_pipeline_ = device_->CreateGraphicsPipeline(desc);
  }

  // Composite pipeline: fullscreen albedo*light over rx's scene, alpha blended.
  {
    base::Vector<PipelineBindings> sets;
    sets.push_back({.slots = {{0, BindingType::kCombinedTextureSampler},
                              {1, BindingType::kCombinedTextureSampler}}});
    GraphicsPipelineDesc desc{
        .vertex = Spirv(k_composite_vs_hlsl, sizeof(k_composite_vs_hlsl)),
        .fragment = Spirv(k_composite_ps_hlsl, sizeof(k_composite_ps_hlsl)),
        .topology = PrimitiveTopology::kTriangleList,
        .raster = {.cull = CullMode::kNone},
        .depth = {},
        .color_formats = {kTargetFormat},
        .blend = {BlendMode::kAlpha},
        .sets = sets,
        .push_constant_size = 0,
        .debug_name = "render2d_composite",
    };
    composite_pipeline_ = device_->CreateGraphicsPipeline(desc);
  }

  return sprite_pipeline_ && light_pipeline_ && composite_pipeline_;
}

TextureId SpriteRenderer::CreateTexture(u32 width, u32 height, const u8* rgba,
                                        const char* debug_name) {
  if (!device_ || width == 0 || height == 0 || !rgba) return 0;
  GpuImage image = device_->CreateImage2D(
      Format::kRGBA8Srgb, {width, height},
      kTextureUsageSampled | kTextureUsageTransferDst, /*mip_levels=*/1);
  if (!image) {
    RX_ERROR("render2d: texture {}x{} creation failed", width, height);
    return 0;
  }

  const u64 bytes = static_cast<u64>(width) * height * 4u;
  GpuBuffer staging = device_->CreateBuffer(bytes, kBufferUsageTransferSrc, /*host_visible=*/true);
  if (!staging.mapped) {
    RX_ERROR("render2d: texture staging failed");
    device_->DestroyImage(image);
    return 0;
  }
  std::memcpy(staging.mapped, rgba, bytes);

  device_->ImmediateSubmit([&](CommandList& cmd) {
    cmd.Barrier(Transition(image, ResourceState::kUndefined, ResourceState::kCopyDst));
    BufferTextureCopy region{.buffer_offset = 0, .mip = 0, .array_layer = 0,
                             .extent = {width, height}};
    cmd.CopyBufferToTexture(staging, image, {&region, 1});
    cmd.Barrier(Transition(image, ResourceState::kCopyDst, ResourceState::kShaderReadFragment));
  });
  device_->DestroyBuffer(staging);
  (void)debug_name;

  textures_.push_back({.image = image, .valid = true});
  return static_cast<TextureId>(textures_.size() - 1);
}

void SpriteRenderer::DestroyTexture(TextureId id) {
  if (id == 0 || id >= textures_.size() || !textures_[id].valid) return;
  device_->WaitIdle();
  device_->DestroyImage(textures_[id].image);
  textures_[id].valid = false;
}

void SpriteRenderer::Begin(const Camera2D& camera) {
  view_proj_ = camera.ViewProj();
  queue_.clear();
  lights_.clear();
}

void SpriteRenderer::DrawSprite(const SpriteParams& s) {
  QueuedSprite q;
  q.gpu.pos[0] = s.pos.x;
  q.gpu.pos[1] = s.pos.y;
  q.gpu.size[0] = s.size.x;
  q.gpu.size[1] = s.size.y;
  q.gpu.uv_min[0] = s.uv.x;
  q.gpu.uv_min[1] = s.uv.y;
  q.gpu.uv_max[0] = s.uv.x + s.uv.w;
  q.gpu.uv_max[1] = s.uv.y + s.uv.h;
  q.gpu.color[0] = s.color.r;
  q.gpu.color[1] = s.color.g;
  q.gpu.color[2] = s.color.b;
  q.gpu.color[3] = s.color.a;
  q.gpu.depth = s.depth;
  q.gpu.rotation = s.rotation;
  q.gpu.pad[0] = q.gpu.pad[1] = 0;
  q.texture = (s.texture != 0 && s.texture < textures_.size() && textures_[s.texture].valid)
                  ? s.texture
                  : kWhiteTexture;
  q.sort_key = s.sort_key;
  queue_.push_back(q);
}

void SpriteRenderer::DrawQuad(Vec2 pos, Vec2 size, Color color, f32 sort_key, f32 depth) {
  SpriteParams s;
  s.pos = pos;
  s.size = size;
  s.color = color;
  s.sort_key = sort_key;
  s.depth = depth;
  s.texture = kWhiteTexture;
  DrawSprite(s);
}

void SpriteRenderer::DrawTileMap(TextureId tileset_texture, const TileMap& map,
                                 const Camera2D& camera) {
  Rect view = camera.VisibleRect();
  for (const TileLayer& layer : map.layers) {
    // Parallax shifts a distant layer so it scrolls slower than the camera.
    Vec2 offset = camera.center() * (1.0f - layer.parallax);
    // Visible tile range in this layer's own space (undo the parallax offset).
    f32 x0 = view.x - offset.x, y0 = view.y - offset.y;
    i32 tx0 = static_cast<i32>(std::floor(x0 / map.tile_size)) - 1;
    i32 ty0 = static_cast<i32>(std::floor(y0 / map.tile_size)) - 1;
    i32 tx1 = static_cast<i32>(std::floor((x0 + view.w) / map.tile_size)) + 1;
    i32 ty1 = static_cast<i32>(std::floor((y0 + view.h) / map.tile_size)) + 1;
    tx0 = std::max(tx0, 0);
    ty0 = std::max(ty0, 0);
    tx1 = std::min(tx1, static_cast<i32>(layer.width) - 1);
    ty1 = std::min(ty1, static_cast<i32>(layer.height) - 1);
    for (i32 ty = ty0; ty <= ty1; ++ty) {
      for (i32 tx = tx0; tx <= tx1; ++tx) {
        i32 id = layer.At(tx, ty);
        if (id < 0) continue;
        SpriteParams s;
        s.pos = {static_cast<f32>(tx) * map.tile_size + offset.x,
                 static_cast<f32>(ty) * map.tile_size + offset.y};
        s.size = {map.tile_size, map.tile_size};
        s.uv = map.tileset.TileUv(id);
        s.texture = tileset_texture;
        s.sort_key = layer.depth;
        s.depth = layer.depth;
        DrawSprite(s);
      }
    }
  }
}

void SpriteRenderer::AddLight(const Light2D& l) {
  Vec2 d = l.dir;
  if (d.x != 0 || d.y != 0) d = Normalize(d);
  GpuLight g;
  g.center[0] = l.center.x;
  g.center[1] = l.center.y;
  g.radius = l.radius;
  g.intensity = l.intensity;
  g.color[0] = l.color.r;
  g.color[1] = l.color.g;
  g.color[2] = l.color.b;
  g.color[3] = l.color.a;
  g.dir[0] = d.x;
  g.dir[1] = d.y;
  g.cone = l.cone;
  g.falloff = l.falloff;
  lights_.push_back(g);
}

void SpriteRenderer::InstallInto(render::FrameView& view) {
  if (!ready_) return;
  view.scene_opaque = [this](const SceneHookContext& ctx) { Record(ctx); };
}

bool SpriteRenderer::EnsureSprites(FrameSlot& slot, u32 count) {
  if (slot.sprite_cap >= count && slot.sprites.mapped) return true;
  u32 cap = std::max<u32>(count, 256);
  cap = cap + cap / 2;  // headroom
  if (slot.sprites) device_->DestroyBufferDeferred(slot.sprites);
  slot.sprites = device_->CreateBuffer(static_cast<u64>(cap) * sizeof(GpuSprite),
                                       kBufferUsageStorage, /*host_visible=*/true);
  slot.sprite_cap = slot.sprites.mapped ? cap : 0;
  return slot.sprites.mapped != nullptr;
}

bool SpriteRenderer::EnsureLights(FrameSlot& slot, u32 count) {
  if (slot.light_cap >= count && slot.lights.mapped) return true;
  u32 cap = std::max<u32>(count, 64);
  cap = cap + cap / 2;
  if (slot.lights) device_->DestroyBufferDeferred(slot.lights);
  slot.lights = device_->CreateBuffer(static_cast<u64>(cap) * sizeof(GpuLight),
                                      kBufferUsageStorage, /*host_visible=*/true);
  slot.light_cap = slot.lights.mapped ? cap : 0;
  return slot.lights.mapped != nullptr;
}

bool SpriteRenderer::EnsureLitTargets(FrameSlot& slot, render::Extent2D extent) {
  if (slot.albedo && slot.light_target && slot.lit_extent == extent) return true;
  if (slot.albedo) device_->DestroyImageDeferred(slot.albedo);
  if (slot.light_target) device_->DestroyImageDeferred(slot.light_target);
  TextureUsageFlags usage = kTextureUsageSampled | kTextureUsageColorTarget;
  slot.albedo = device_->CreateImage2D(kTargetFormat, extent, usage);
  slot.light_target = device_->CreateImage2D(kTargetFormat, extent, usage);
  slot.lit_extent = extent;
  return slot.albedo && slot.light_target;
}

u32 SpriteRenderer::UploadSprites(FrameSlot& slot) {
  u32 count = static_cast<u32>(queue_.size());
  if (count == 0) return 0;
  // Painter's order by sort_key; group by texture within a band so runs of the
  // same atlas collapse to one instanced draw.
  std::stable_sort(queue_.data(), queue_.data() + count,
                   [](const QueuedSprite& a, const QueuedSprite& b) {
                     if (a.sort_key != b.sort_key) return a.sort_key < b.sort_key;
                     return a.texture < b.texture;
                   });
  if (!EnsureSprites(slot, count)) return 0;
  GpuSprite* dst = static_cast<GpuSprite*>(slot.sprites.mapped);
  for (u32 i = 0; i < count; ++i) dst[i] = queue_[i].gpu;
  return count;
}

void SpriteRenderer::DrawSpriteRuns(render::CommandList& cmd, FrameSlot& slot, u32 count) {
  cmd.BindPipeline(sprite_pipeline_);
  cmd.Push(view_proj_);
  u32 i = 0;
  while (i < count) {
    TextureId tex = queue_[i].texture;
    u32 start = i;
    while (i < count && queue_[i].texture == tex) ++i;
    u32 run = i - start;
    const Texture& t = textures_[tex];
    cmd.BindTransient(0, {Bind::StorageBuffer(0, slot.sprites),
                          Bind::Combined(1, t.image.view, sampler_)});
    cmd.Draw(6, run, 0, start);
  }
}

void SpriteRenderer::Record(const SceneHookContext& ctx) {
  if (ctx.phase != ScenePhase::kOpaque) return;
  FrameSlot& slot = slots_[ctx.frame_slot % 2];
  if (lighting_ == LightingMode::kLit) {
    RecordLit(ctx, slot);
  } else {
    RecordUnlit(ctx, slot);
  }
}

void SpriteRenderer::RecordUnlit(const SceneHookContext& ctx, FrameSlot& slot) {
  u32 count = UploadSprites(slot);
  ColorAttachment color{.view = ctx.color_view,
                        .load = clear_scene_ ? LoadOp::kClear : LoadOp::kLoad};
  if (clear_scene_) {
    color.clear[0] = scene_clear_.r;
    color.clear[1] = scene_clear_.g;
    color.clear[2] = scene_clear_.b;
    color.clear[3] = scene_clear_.a;
  }
  ctx.cmd->BeginRendering({.extent = ctx.extent, .colors = {&color, 1}});
  if (count > 0) DrawSpriteRuns(*ctx.cmd, slot, count);
  ctx.cmd->EndRendering();
}

void SpriteRenderer::RecordLit(const SceneHookContext& ctx, FrameSlot& slot) {
  if (!EnsureLitTargets(slot, ctx.extent)) {
    RecordUnlit(ctx, slot);  // fall back if the offscreen targets failed
    return;
  }
  u32 sprite_count = UploadSprites(slot);
  u32 light_count = static_cast<u32>(lights_.size());
  if (light_count > 0 && EnsureLights(slot, light_count)) {
    std::memcpy(slot.lights.mapped, lights_.data(), light_count * sizeof(GpuLight));
  } else {
    light_count = 0;
  }

  // Pass A: unlit sprite albedo into the albedo target (cleared transparent).
  ctx.cmd->Barrier(Transition(slot.albedo, ResourceState::kUndefined, ResourceState::kColorTarget));
  {
    ColorAttachment a{.view = slot.albedo.view, .load = LoadOp::kClear, .clear = {0, 0, 0, 0}};
    ctx.cmd->BeginRendering({.extent = ctx.extent, .colors = {&a, 1}});
    if (sprite_count > 0) DrawSpriteRuns(*ctx.cmd, slot, sprite_count);
    ctx.cmd->EndRendering();
  }
  ctx.cmd->Barrier(
      Transition(slot.albedo, ResourceState::kColorTarget, ResourceState::kShaderReadFragment));

  // Pass B: additive light accumulation over the ambient clear.
  ctx.cmd->Barrier(
      Transition(slot.light_target, ResourceState::kUndefined, ResourceState::kColorTarget));
  {
    ColorAttachment lc{.view = slot.light_target.view,
                       .load = LoadOp::kClear,
                       .clear = {ambient_.r, ambient_.g, ambient_.b, 1.0f}};
    ctx.cmd->BeginRendering({.extent = ctx.extent, .colors = {&lc, 1}});
    if (light_count > 0) {
      ctx.cmd->BindPipeline(light_pipeline_);
      ctx.cmd->Push(view_proj_);
      ctx.cmd->BindTransient(0, {Bind::StorageBuffer(0, slot.lights)});
      ctx.cmd->Draw(6, light_count, 0, 0);
    }
    ctx.cmd->EndRendering();
  }
  ctx.cmd->Barrier(Transition(slot.light_target, ResourceState::kColorTarget,
                              ResourceState::kShaderReadFragment));

  // Pass C: composite albedo*light over rx's scene colour.
  ColorAttachment sc{.view = ctx.color_view,
                     .load = clear_scene_ ? LoadOp::kClear : LoadOp::kLoad};
  if (clear_scene_) {
    sc.clear[0] = scene_clear_.r;
    sc.clear[1] = scene_clear_.g;
    sc.clear[2] = scene_clear_.b;
    sc.clear[3] = scene_clear_.a;
  }
  ctx.cmd->BeginRendering({.extent = ctx.extent, .colors = {&sc, 1}});
  ctx.cmd->BindPipeline(composite_pipeline_);
  ctx.cmd->BindTransient(0, {Bind::Combined(0, slot.albedo.view, sampler_),
                             Bind::Combined(1, slot.light_target.view, sampler_)});
  ctx.cmd->Draw(3, 1, 0, 0);
  ctx.cmd->EndRendering();
}

void SpriteRenderer::Shutdown() {
  if (!device_) return;
  device_->WaitIdle();
  if (sprite_pipeline_) device_->DestroyPipeline(sprite_pipeline_);
  if (light_pipeline_) device_->DestroyPipeline(light_pipeline_);
  if (composite_pipeline_) device_->DestroyPipeline(composite_pipeline_);
  sprite_pipeline_ = light_pipeline_ = composite_pipeline_ = {};
  for (FrameSlot& slot : slots_) {
    if (slot.sprites) device_->DestroyBuffer(slot.sprites);
    if (slot.lights) device_->DestroyBuffer(slot.lights);
    if (slot.albedo) device_->DestroyImage(slot.albedo);
    if (slot.light_target) device_->DestroyImage(slot.light_target);
    slot = {};
  }
  for (Texture& t : textures_) {
    if (t.valid) device_->DestroyImage(t.image);
    t.valid = false;
  }
  textures_.clear();
  device_ = nullptr;
  renderer_ = nullptr;
  ready_ = false;
}

}  // namespace rx::render2d
