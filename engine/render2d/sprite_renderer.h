#ifndef RX_RENDER2D_SPRITE_RENDERER_H_
#define RX_RENDER2D_SPRITE_RENDERER_H_

#include <base/containers/vector.h>

#include "core/math.h"
#include "render/core/renderer.h"
#include "render/rhi/device.h"
#include "render2d/camera2d.h"
#include "render2d/tile_map.h"
#include "render2d/types2d.h"

namespace rx::render2d {

// Opaque texture id. 0 is invalid; kWhiteTexture (1) is a built-in 1x1 white
// texel for solid-colour quads.
using TextureId = u32;
inline constexpr TextureId kWhiteTexture = 1;

// A 2D light for the lit path: a radial pool with optional spot cone. Positions
// are in the same y-down world space as Camera2D / sprites.
struct Light2D {
  Vec2 center{};
  f32 radius = 64.0f;
  f32 intensity = 1.0f;
  Color color = Color::White();
  Vec2 dir{0, 0};    // spot facing (need not be normalized); (0,0) = omni
  f32 cone = -1.0f;  // cos(half-angle) for a spot; <= -1 = omni
  f32 falloff = 1.5f;
};

// One queued sprite. pos is the top-left corner; uv is the atlas sub-rect
// (origin+extent in 0..1). sort_key drives draw order (ascending, painter's);
// ties keep submission order.
struct SpriteParams {
  Vec2 pos{};
  Vec2 size{};
  Rect uv{0, 0, 1, 1};
  Color color = Color::White();
  f32 rotation = 0.0f;
  f32 depth = 0.5f;
  TextureId texture = kWhiteTexture;
  f32 sort_key = 0.0f;
};

enum class LightingMode : u8 { kOff, kLit };

// GPU-facing instance layouts. Field order/size mirror the HLSL structs
// (std430); the static_asserts in the .cc guard the match.
struct GpuSprite {
  f32 pos[2];
  f32 size[2];
  f32 uv_min[2];
  f32 uv_max[2];
  f32 color[4];
  f32 depth;
  f32 rotation;
  f32 pad[2];
};

struct GpuLight {
  f32 center[2];
  f32 radius;
  f32 intensity;
  f32 color[4];
  f32 dir[2];
  f32 cone;
  f32 falloff;
};

// The reusable 2D sprite / tilemap / lighting renderer. It records into rx's
// frame through the scene-opaque hook using its own orthographic matrices, so
// the 2D content is depth-interleaved with (and post-processed by) rx: bloom,
// tonemap and the rest of rx's post chain run over it. Own one per game/scene;
// drive it each frame: Begin(camera) -> Draw*() -> InstallInto(view).
class SpriteRenderer {
 public:
  SpriteRenderer() = default;
  ~SpriteRenderer() { Shutdown(); }

  SpriteRenderer(const SpriteRenderer&) = delete;
  SpriteRenderer& operator=(const SpriteRenderer&) = delete;

  // Creates pipelines/samplers/built-in textures. Returns false (and stays
  // inert) on a headless / stub / non-Vulkan device.
  bool Init(render::Renderer& renderer);
  void Shutdown();
  bool ready() const { return ready_; }

  // Upload an RGBA8 image as an sRGB texture (so the sampler returns linear).
  // Returns a texture id, or 0 on failure. `rgba` is width*height*4 bytes.
  TextureId CreateTexture(u32 width, u32 height, const u8* rgba, const char* debug_name = nullptr);
  void DestroyTexture(TextureId id);

  // --- per frame (call in OnBuildView) ---
  void Begin(const Camera2D& camera);
  void DrawSprite(const SpriteParams& s);
  void DrawQuad(Vec2 pos, Vec2 size, Color color, f32 sort_key = 0.0f, f32 depth = 0.5f);
  // Emits every visible non-empty tile of every layer (parallax-aware).
  void DrawTileMap(TextureId tileset_texture, const TileMap& map, const Camera2D& camera);

  // Lighting. kLit routes through an albedo->light->composite chain; kOff draws
  // straight into rx's scene colour. Ambient is the light target's clear colour.
  void SetLightingMode(LightingMode m) { lighting_ = m; }
  LightingMode lighting_mode() const { return lighting_; }
  void SetAmbient(Color c) { ambient_ = c; }
  void AddLight(const Light2D& l);

  // Background handling for the scene target. When set, the pass clears rx's
  // scene colour to this colour before compositing the 2D content (pure-2D
  // slices); unset keeps rx's content (kLoad) so 2D composites over 3D.
  void SetSceneClear(Color bg) {
    clear_scene_ = true;
    scene_clear_ = bg;
  }
  void ClearSceneClear() { clear_scene_ = false; }

  // Installs this frame's queued content as view.scene_opaque.
  void InstallInto(render::FrameView& view);

 private:
  struct Texture {
    render::GpuImage image;
    bool valid = false;
  };
  struct FrameSlot {
    render::GpuBuffer sprites;  // host-visible structured buffer of GpuSprite
    render::GpuBuffer lights;   // host-visible structured buffer of GpuLight
    u32 sprite_cap = 0;
    u32 light_cap = 0;
    render::GpuImage albedo;  // lit path, sized to the render extent
    render::GpuImage light_target;
    render::Extent2D lit_extent{};
  };
  struct QueuedSprite {
    GpuSprite gpu;
    TextureId texture;
    f32 sort_key;
  };

  bool CreatePipelines();
  void Record(const render::SceneHookContext& ctx);
  void RecordUnlit(const render::SceneHookContext& ctx, FrameSlot& slot);
  void RecordLit(const render::SceneHookContext& ctx, FrameSlot& slot);
  bool EnsureSprites(FrameSlot& slot, u32 count);
  bool EnsureLights(FrameSlot& slot, u32 count);
  bool EnsureLitTargets(FrameSlot& slot, render::Extent2D extent);
  u32 UploadSprites(FrameSlot& slot);  // sorts + writes; returns count
  void DrawSpriteRuns(render::CommandList& cmd, FrameSlot& slot, u32 count);

  render::Renderer* renderer_ = nullptr;
  render::Device* device_ = nullptr;
  bool ready_ = false;

  render::PipelineHandle sprite_pipeline_;
  render::PipelineHandle light_pipeline_;
  render::PipelineHandle composite_pipeline_;
  render::SamplerHandle sampler_;  // linear clamp

  base::Vector<Texture> textures_;  // index 0 unused, 1 = white

  Mat4 view_proj_ = Mat4::Identity();
  base::Vector<QueuedSprite> queue_;
  base::Vector<GpuLight> lights_;
  LightingMode lighting_ = LightingMode::kOff;
  Color ambient_ = Color::White();
  bool clear_scene_ = false;
  Color scene_clear_ = Color::Black();

  FrameSlot slots_[2];
};

}  // namespace rx::render2d

#endif  // RX_RENDER2D_SPRITE_RENDERER_H_
