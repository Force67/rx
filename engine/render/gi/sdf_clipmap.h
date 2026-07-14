#ifndef RX_RENDER_GI_SDF_CLIPMAP_H_
#define RX_RENDER_GI_SDF_CLIPMAP_H_

#include <base/containers/vector.h>

#include "core/math.h"
#include "core/types.h"
#include "render/core/render_graph.h"
#include "render/rhi/device.h"
#include "render/rhi/resources.h"

namespace rx::render {

class SdfScene;

// Camera-following global SDF clipmap with a surface-colour proxy, mirroring the
// RCGI cascade geometry. Four clips double in extent (clip 0 = 32 m ... clip 3 =
// 256 m), each a kRes^3 voxel volume, camera-snapped. Per clip the composition
// pass min-blends the mesh SDFs of overlapping instances into a distance volume,
// and the winning instance writes its flat albedo/emissive into two colour
// proxies. S2 sphere-traces this via shaders/gi/sdf_trace.hlsli.
//
// The three volumes are one stacked-Z atlas (kRes, kRes, kRes*kClips): clip c
// occupies z in [c*kRes, (c+1)*kRes). They live permanently in kGeneral and are
// hand-barriered (the RcgiSystem discipline). Everything is gated by RX_SDF: the
// renderer only builds a clipmap when the software-trace path is on.
class SdfClipmap {
 public:
  static constexpr u32 kRes = 128;         // per-axis voxels per clip (tier knob)
  static constexpr u32 kClips = 4;         // mirrors RCGI cascades
  static constexpr f32 kBaseExtent = 32.0f;  // clip 0 world extent (m); doubles per clip
  static constexpr f32 kFarDistance = 1.0e4f;  // empty-space sentinel distance

  struct Instance {
    u64 mesh_key = 0;
    Mat4 transform = Mat4::Identity();
  };

  explicit SdfClipmap(Device& device) : device_(device) {}
  ~SdfClipmap();

  SdfClipmap(const SdfClipmap&) = delete;
  SdfClipmap& operator=(const SdfClipmap&) = delete;

  // Creates the volumes + pipelines. Returns false if 3D storage images are
  // unavailable (the caller then drops the SDF path and renders without it).
  bool Initialize();

  // Round-robin recomposition (one clip/frame + any clip that snapped to a new
  // cell); rebuilds every clip on the first frame / a teleport. Adds the clear +
  // per-instance compose passes; the volumes stay in kGeneral.
  void AddComposeToGraph(RenderGraph& graph, const SdfScene& scene,
                         base::Vector<Instance> instances, const Vec3& camera, u32 frame_index);

  // Full-screen raymarch of the clipmap onto `lit` (RX_SDF_DEBUG: 1 distance,
  // 2 albedo+normal). Verification tool; also proof the trace/data are correct.
  void AddDebugPass(RenderGraph& graph, ResourceHandle lit, Extent2D extent,
                    const Mat4& inv_view_proj, const Vec3& camera, u32 mode, u32 frame_index);

  // --- S2 interface (bind these into the software trace variants) ---
  const GpuImage& distance_volume() const { return distance_; }
  const GpuImage& albedo_volume() const { return albedo_; }
  const GpuImage& emissive_volume() const { return emissive_; }
  SamplerHandle sampler() const { return sampler_; }
  const GpuBuffer& globals(u32 frame_index) const { return globals_buffers_[frame_index % 2]; }
  bool ready() const { return volumes_initialized_; }

 private:
  Vec3 SnapOrigin(const Vec3& camera, u32 clip) const;
  void WriteGlobals(u32 frame_index, const Vec3& camera);

  Device& device_;
  GpuImage distance_;   // R16Float, signed distance
  GpuImage albedo_;     // RGBA8Unorm, surface albedo proxy
  GpuImage emissive_;   // RGBA8Unorm, surface emissive proxy
  SamplerHandle sampler_{};
  GpuBuffer globals_buffers_[2];  // host-visible SdfGlobals, ping-pong by frame parity

  PipelineHandle clear_pipeline_{};
  PipelineHandle compose_pipeline_{};
  PipelineHandle debug_pipeline_{};

  bool volumes_initialized_ = false;
  bool clips_valid_ = false;
  Vec3 clip_origin_[kClips] = {};  // world min corner each clip's data represents
};

}  // namespace rx::render

#endif  // RX_RENDER_GI_SDF_CLIPMAP_H_
