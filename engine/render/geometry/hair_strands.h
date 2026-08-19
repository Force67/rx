#ifndef RX_RENDER_HAIR_STRANDS_H_
#define RX_RENDER_HAIR_STRANDS_H_

// Strand-based hair display: guide strands simulated elsewhere (the physics
// module's Jolt soft-body strand grooms) are rendered as camera-facing ribbons
// expanded in the vertex shader straight from a node-position buffer, shaded
// with a dual-lobe Kajiya-Kay specular along the strand tangent. Grooms are
// built from real hair meshes (see hair_groom.h); several can coexist, each
// fed fresh node positions per frame through SetGroomPoints (a groom that is
// never fed keeps its uploaded rest pose).

#include <base/containers/vector.h>

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/geometry/hair_groom.h"
#include "render/pipeline/hair_material.h"
#include "render/rhi/device.h"

namespace rx::render {

class HairStrands {
 public:
  bool Initialize(Device& device, Format color_format, Format depth_format);
  void Destroy(Device& device);
  bool active() const;

  // Uploads a CPU-built groom and returns a stable handle (0 = failure). The
  // transform places the groom-local frame (scalp at origin) into the world.
  u32 CreateGroom(Device& device, const GroomData& data, const GroomParams& params,
                  const Mat4& transform);
  // Re-places the groom. A groom that no simulation feeds follows rigidly
  // (its rest pose re-transformed); a fed groom only moves its head sphere,
  // the node positions stay owned by the feed.
  void SetGroomTransform(u32 id, const Mat4& transform);
  void SetGroomTint(u32 id, const Vec3& tint);
  // Feeds this frame's simulated node positions (world-space xyz, strand-major,
  // guide_count * kGroomPointsPerStrand nodes). Extra floats are ignored.
  void SetGroomPoints(u32 id, const f32* positions, u32 count);
  void DestroyGroom(Device& device, u32 id);
  // The groom's head collision sphere in world space (transform applied), for
  // aligning a head mesh to it. Returns false for an unknown id.
  bool GroomHead(u32 id, Vec3* center, f32* radius);

  // Procedural fallback groom on a head sphere (the original --demo strands
  // look); static unless a simulation feeds it through SetGroomPoints.
  void SeedCap(Device& device, const Vec3& head_center, f32 head_radius, u32 strand_count,
               f32 strand_length);

  // Per-groom fibre parameters. Colour is PIGMENT: the groom's per-strand colour
  // tints it, it does not replace it. See render/pipeline/hair_material.h.
  void SetGroomHair(u32 id, const HairSurfaceParameters& params);
  void SetGroomTier(u32 id, HairTier tier);

  struct Frame {
    Mat4 view_proj;
    Vec3 camera_pos;
    Vec3 sun_direction;  // travel
    f32 sun_intensity = 3.0f;
    Vec3 sun_color{1, 1, 1};
    // Sky/ambient reaching the groom. Hair with no ambient reads as a black
    // silhouette the moment it leaves the sun, which is not what hair does.
    Vec3 ambient{0.05f, 0.055f, 0.06f};
    // Metres the deep opacity map's four layers span past the front-most fibre.
    // Roughly the depth of the groom; too small and the interior saturates at
    // the first layer, too large and the front fibres stop resolving.
    f32 transmittance_depth = 0.18f;
    // Rendered ribbons -> optical fibres. A groom draws `children` ribbons per
    // simulated guide, and the DOM counts ribbons; this is the correction.
    f32 fibre_scale = 1.0f;
    // How much sun one crossed fibre removes from a surface UNDER the groom.
    // A fibre is nearly opaque to a ray that hits it, but the volume counts
    // partial ribbon coverage, so this is below 1.
    f32 shadow_density = 0.6f;
    // render::DebugView. The strand pass honours the fibre-count view: a debug
    // view occluded by the very geometry it is diagnosing is not a diagnostic.
    u32 debug_view = 0;
    bool transmittance = true;
  };

  // Renders the deep opacity map from the sun: pass one records the front-most
  // fibre depth per light texel, pass two accumulates fibre counts into four
  // layers past it. Must run before AddToGraph. No-op with no live grooms.
  void AddTransmittanceToGraph(RenderGraph& graph, const Frame& frame, u32 frame_slot);

  void AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                  Extent2D extent, const Frame& frame, u32 frame_slot);

  // The volume the last AddTransmittanceToGraph produced, for consumers outside
  // the hair pass (the skin under the groom needs the same fibre count). Null
  // view when hair is absent or the volume is off.
  struct TransmittanceBinding {
    TextureView front_depth;
    TextureView layers;
    const GpuBuffer* params = nullptr;
    SamplerHandle sampler;
  };
  TransmittanceBinding transmittance() const;

  static constexpr u32 kFramesInFlight = 2;
  static constexpr u32 kTransmittanceResolution = 1024;

 private:
  struct HairPoint {
    f32 pos[4];   // xyz, w unused (layout shared with hair.vs)
    f32 prev[4];  // xyz previous position, w unused
  };
  struct Groom {
    GpuBuffer points[kFramesInFlight];  // host-visible HairPoint ring
    GpuBuffer colors;                   // guide_count float4 linear rgb
    GpuBuffer indices;  // ribbon triangle list over guide_count * children strands
    base::Vector<HairPoint> host_points;  // canonical CPU copy, world space
    base::Vector<f32> local_points;       // groom-local rest, for the unfed rigid path
    u32 stale = 0;      // per-slot bits: host_points newer than points[slot]
    bool fed = false;   // a simulation has supplied points at least once
    u32 guide_count = 0;
    u32 children = 1;
    u32 index_count = 0;
    f32 strand_width = 0;
    f32 clump_radius = 0;
    Mat4 transform;
    Vec3 collision_center{};
    f32 collision_radius = 0;
    Vec3 tint{1, 1, 1};
    HairSurfaceParameters hair;
    HairTier tier = HairTier::kHero;
    GpuBuffer material;  // GroomMaterial, rewritten only when the material changes
    u32 id = 0;
    bool alive = false;
  };
  Groom* Find(u32 id);
  u32 Upload(Device& device, const GroomData& data, const GroomParams& params,
             const Mat4& transform);
  // World AABB of every live groom's points. The light frustum is fitted to it,
  // so the map's resolution follows the hair rather than the world.
  bool WorldBounds(Vec3* lo, Vec3* hi) const;
  void WriteGroomMaterial(Groom& g);

  PipelineHandle draw_pipeline_;
  PipelineHandle depth_pipeline_;  // DOM pass one: front-most fibre depth
  PipelineHandle dom_pipeline_;    // DOM pass two: layered fibre counts
  SamplerHandle volume_sampler_;
  GpuImage front_depth_;
  GpuImage dom_;
  GpuBuffer volume_params_[kFramesInFlight];
  ResourceState front_depth_state_ = ResourceState::kUndefined;
  ResourceState dom_state_ = ResourceState::kUndefined;
  bool volume_valid_ = false;
  u32 volume_slot_ = 0;
  ResourceHandle volume_front_handle_{};
  ResourceHandle volume_layers_handle_{};
  Device* device_ = nullptr;
  base::Vector<Groom> grooms_;
  u32 next_id_ = 1;
};

}  // namespace rx::render

#endif  // RX_RENDER_HAIR_STRANDS_H_
