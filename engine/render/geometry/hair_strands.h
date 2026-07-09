#ifndef RX_RENDER_HAIR_STRANDS_H_
#define RX_RENDER_HAIR_STRANDS_H_

// Strand-based hair: individually simulated guide strands (verlet particles with
// inextensibility constraints, gravity, wind and head-sphere collision, one
// compute thread per strand) rendered as camera-facing ribbons expanded in the
// vertex shader straight from the simulation buffer, shaded with a dual-lobe
// Kajiya-Kay specular along the strand tangent. Grooms are built from real hair
// meshes (see hair_groom.h); several can coexist, each rigidly attached to a
// moving transform (a head bone) with a collision sphere that follows it.

#include <base/containers/vector.h>

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/geometry/hair_groom.h"
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
  void SetGroomTransform(u32 id, const Mat4& transform);
  void SetGroomTint(u32 id, const Vec3& tint);
  void DestroyGroom(Device& device, u32 id);
  // The groom's head collision sphere in world space (transform applied), for
  // aligning a head mesh to it. Returns false for an unknown id.
  bool GroomHead(u32 id, Vec3* center, f32* radius);

  // Procedural fallback groom on a head sphere (the original --demo strands look).
  void SeedCap(Device& device, const Vec3& head_center, f32 head_radius, u32 strand_count,
               f32 strand_length);

  struct Frame {
    Mat4 view_proj;
    Vec3 camera_pos;
    f32 delta_seconds = 0.016f;
    Vec3 sun_direction;  // travel
    f32 sun_intensity = 3.0f;
    Vec3 sun_color{1, 1, 1};
    f32 time = 0.0f;
    f32 wind[3] = {0.4f, 0.0f, 0.2f};
  };

  void AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                  Extent2D extent, const Frame& frame);

 private:
  struct Groom {
    GpuBuffer points;   // guide_count * points_per_strand HairPoints (world)
    GpuBuffer rest;     // guide_count * points_per_strand float4 local rest pose
    GpuBuffer colors;   // guide_count float4 linear rgb
    GpuBuffer indices;  // ribbon triangle list over guide_count * children strands
    u32 guide_count = 0;
    u32 children = 1;
    u32 index_count = 0;
    f32 strand_width = 0;
    f32 clump_radius = 0;
    Mat4 transform;
    Vec3 collision_center{};
    f32 collision_radius = 0;
    Vec3 tint{1, 1, 1};
    u32 id = 0;
    bool alive = false;
  };
  Groom* Find(u32 id);
  u32 Upload(Device& device, const GroomData& data, const GroomParams& params,
             const Mat4& transform);

  PipelineHandle sim_pipeline_;
  PipelineHandle draw_pipeline_;
  base::Vector<Groom> grooms_;
  u32 next_id_ = 1;
};

}  // namespace rx::render

#endif  // RX_RENDER_HAIR_STRANDS_H_
