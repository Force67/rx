#ifndef RX_RENDER_MESH_PIPELINE_H_
#define RX_RENDER_MESH_PIPELINE_H_

#include <memory>

#include "core/math.h"
#include "render/rhi/device.h"

namespace rx::render {

// A dynamic omni light, accumulated in the forward lighting pass. Packed in
// float4s so the StructuredBuffer stride matches the shader exactly.
// Dynamic local light. The first two rows are the legacy point-light layout;
// direction/params extend it to spots and representative-point area lights.
// type: 0 point, 1 spot, 2 sphere area, 3 rect area.
struct PointLight {
  f32 pos_radius[4] = {0, 0, 0, 1};       // xyz position, w influence radius (meters)
  f32 color_intensity[4] = {1, 1, 1, 1};  // rgb color, w intensity
  f32 direction_type[4] = {0, -1, 0, 0};  // xyz emit direction, w type
  // spot: x cos(inner), y cos(outer). sphere area: x source radius (m).
  // rect area: x,y half extents (m); the basis derives from the direction.
  f32 params[4] = {0.9f, 0.7f, 0, 0};
};

// Per frame camera and lighting state, bound as set 0. Layout matches the
// std140 block in mesh.vert/mesh.frag.
struct FrameGlobals {
  Mat4 view_proj;
  Mat4 prev_view_proj;
  Mat4 inv_view_proj;
  f32 jitter[2] = {0, 0};  // ndc units
  f32 prev_jitter[2] = {0, 0};
  f32 sun_direction[4] = {0, -1, 0, 4};  // xyz travel direction, w intensity
  f32 sun_color[4] = {1, 1, 1, 0.06f};   // rgb color, w flat ambient when ibl off
  f32 camera_position[4] = {0, 0, 0, 1};  // xyz eye, w ibl intensity
  f32 misc[4] = {0, 0, 0, 0};  // x,y render size, z sun angular radius, w frame index
  u32 flags = 0;
  f32 time = 0;  // seconds, drives water waves
  u32 debug_view = 0;  // render::DebugView, isolates a shading channel
  f32 reflection_cutoff = 0.6f;  // roughness above which rt reflections fall back to ibl
  u32 ao_ray_count = 0;  // rt ao rays/pixel this frame (0 when ao is screen-space), for the ray-count view
  u32 light_count = 0;   // dynamic point lights in the bound light buffer
  f32 pad_wind[2] = {0, 0};  // aligns wind to the hlsl float4 register
  // xyz wind direction * strength (m of sway at weight 1), w gust frequency.
  f32 wind[4] = {0.6f, 0.0f, 0.35f, 1.0f};
  // Froxel light clustering: x slice scale, y slice bias (exponential view-z
  // slicing), z,w tile size in pixels.
  f32 cluster_params[4] = {0, 0, 64, 64};
  // Authored interior lighting mode, used when kFrameFlagInterior is set: flat
  // ambient replaces sky IBL, a linear distance fog fades to the fog colours,
  // and the directional fill rides the sun_direction/sun_color path. The app
  // supplies these values (e.g. from an interior/room lighting definition).
  f32 interior_ambient[4] = {0, 0, 0, 0};      // rgb flat ambient (x albedo), w unused
  f32 interior_fog_color0[4] = {0, 0, 0, 0};   // rgb near fog colour, w near dist (m)
  f32 interior_fog_color1[4] = {0, 0, 0, 0};   // rgb far fog colour, w far dist (m)
  f32 interior_fog_params[4] = {1, 1, 0, 0};   // x fog power, y fog max, zw unused
  // Shoreline wetting field (kFrameFlagShoreWetting): xy field min-corner world
  // xz, z 1/extent (1/m), w unused. Zeroed when the feature is off.
  f32 shore_field[4] = {0, 0, 0, 0};
  // Physically-based water shading ([water] settings). Trailing append so the
  // shorter water.ps mirror can extend to reach these; read by water.ps (Beer
  // absorption + crest SSS) and mesh.ps/mesh_rt.ps (underwater caustics).
  // rgb per-channel Beer-Lambert absorption coefficient (1/m), w overall scale.
  f32 water_absorption[4] = {0.35f, 0.045f, 0.028f, 1.0f};
  // x transmission strength, y reflection foam/ripple roughening gain,
  // z crest-SSS intensity, w crest-SSS view exponent.
  f32 water_material[4] = {1.0f, 1.0f, 0.6f, 3.0f};
  // Underwater caustics (kFrameFlagWaterCaustics): x intensity (0..1 lerp),
  // y water rest height (m), z extra depth-fade (1/m), w unused.
  f32 water_caustics[4] = {0.6f, 0.0f, 0.15f, 0.0f};
};

// A projected decal: an oriented box whose -z face carries an atlas region,
// blended into the surface albedo before shading (clustered like the lights).
struct Decal {
  // World -> unit-box rows (decal space: xyz in [-1,1], z along the normal).
  f32 row0[4] = {1, 0, 0, 0};
  f32 row1[4] = {0, 1, 0, 0};
  f32 row2[4] = {0, 0, 1, 0};
  f32 uv_rect[4] = {1, 1, 0, 0};     // atlas uv scale.xy, offset.zw
  f32 tint_blend[4] = {1, 1, 1, 1};  // rgb tint, w albedo blend strength
  // x: normal-map strength (0 = albedo only), y: roughness multiplier the
  // decal's alpha blends toward (1 = unchanged), z: emissive strength
  // (albedo * z adds to the surface emission), w: unused.
  f32 params2[4] = {0, 1, 0, 0};
};

// Froxel grid for clustered lighting/decals (mirrored in the shaders).
inline constexpr u32 kClusterTilesX = 16;
inline constexpr u32 kClusterTilesY = 9;
inline constexpr u32 kClusterSlices = 24;
inline constexpr u32 kClusterCount = kClusterTilesX * kClusterTilesY * kClusterSlices;
inline constexpr u32 kMaxLightsPerCluster = 32;
inline constexpr u32 kMaxDecalsPerCluster = 16;
inline constexpr u32 kMaxFrameDecals = 128;

// FrameGlobals::flags bits, mirrored in mesh.ps.hlsl.
inline constexpr u32 kFrameFlagIbl = 1u << 0;
inline constexpr u32 kFrameFlagAoValid = 1u << 1;
inline constexpr u32 kFrameFlagDdgi = 1u << 2;
inline constexpr u32 kFrameFlagWaterRt = 1u << 3;
inline constexpr u32 kFrameFlagReflections = 1u << 4;  // opaque rt specular reflections
inline constexpr u32 kFrameFlagRtShadows = 1u << 5;    // trace sun shadow in the rt variant
inline constexpr u32 kFrameFlagShadowMap = 1u << 6;    // sample cascaded shadow maps (raster path)
inline constexpr u32 kFrameFlagSigmaShadow = 1u << 7;  // sample the SIGMA-denoised sun shadow
inline constexpr u32 kFrameFlagAurora = 1u << 8;       // draw the night-sky aurora (sky.ps)
inline constexpr u32 kFrameFlagSpecReflTex = 1u << 9;  // sample the denoised reflection target
inline constexpr u32 kFrameFlagRestirDi = 1u << 10;     // point/spot lights come from the ReSTIR DI textures
inline constexpr u32 kFrameFlagFftOcean = 1u << 11;     // water displaces/shades from the FFT ocean maps
inline constexpr u32 kFrameFlagInterior = 1u << 12;    // interior lighting mode: flat ambient + fog, no sky
inline constexpr u32 kFrameFlagWaterField = 1u << 13;    // water samples the persistent foam/ripple ring field
inline constexpr u32 kFrameFlagShoreWetting = 1u << 14;  // sample the shoreline wetting field (env slot 33)
inline constexpr u32 kFrameFlagWaterCaustics = 1u << 15;  // modulate sun on submerged surfaces by the caustic map (env slot 34)
inline constexpr u32 kFrameFlagRcgi = 1u << 16;  // indirect diffuse from the RCGI resolve texture (env slot 35), replaces DDGI + SSGI

// One active morph target on a draw: index into the mesh's target list and
// its weight. FrameView::morph_weights concatenates every morphed draw's
// nonzero set, so untouched targets cost nothing in the vertex shader.
struct MorphWeight {
  u32 target = 0;
  f32 weight = 0;
};

// model + prev_model are 128 bytes; skinned draws append the bone palette's
// buffer device address and this mesh's offset into it (needs a 144 byte push
// range, available on every desktop GPU). Morphed draws use the trailing block
// (192 bytes total). Shaders ignore the tails they do not read.
struct MeshPushConstants {
  Mat4 model;
  Mat4 prev_model;
  u64 bone_address = 0;  // device address of the frame bone palette, 0 = none
  u32 skin_offset = 0;   // first bone of this mesh in the palette
  u32 tint_packed = 0;   // per-draw rgb8 tint (0xRRGGBB) modulating albedo, 0 = none
  // World-space rect (min_x, min_z, max_x, max_z) where full-detail terrain is
  // streamed in. Set only on distant terrain-LOD draws: the vertex shader sinks
  // vertices inside it so the coarse proxy never bridges above the real land
  // (it cut through buildings otherwise). All zeros = no clip.
  f32 detail_rect[4] = {0, 0, 0, 0};
  // Morph targets: per-mesh deltas (GpuMesh::morph_deltas) and the frame's
  // (target, weight) pair buffer, read by device address like the bones.
  // morph_count = 0 disables morphing for the draw.
  u64 morph_delta_address = 0;
  u64 morph_weight_address = 0;
  u32 morph_first = 0;         // this draw's first pair in the weight buffer
  u32 morph_count = 0;         // active (nonzero-weight) pairs
  u32 morph_vertex_count = 0;  // vertices per target in the delta buffer
  u32 pad_morph = 0;
};

// Push constants for the optional mesh-shader opaque path. The geometry buffers
// are read in the mesh shader by device address; layout matches mesh_scene.ms.
struct MeshShaderPush {
  Mat4 model;
  Mat4 prev_model;
  f32 bounds[4] = {0, 0, 0, 0};  // xyz model-space center, w radius (task-stage cull)
  f32 occlusion[4] = {0, 0, 0, 0};  // proj.m00, proj.m11, hiz w, hiz h (w==0 disables)
  u64 meshlets_address = 0;
  u64 meshlet_vertices_address = 0;
  u64 meshlet_triangles_address = 0;
  u64 vertices_address = 0;
  u32 meshlet_offset = 0;
  u32 meshlet_count = 0;
};

// Forward pbr pipeline: classic vertex buffer, metallic roughness shading,
// reversed z depth. Outputs hdr color and motion vectors. Set 0 is the frame
// globals (plus the TLAS when raytracing is available), set 1 the material.
// Variants cover ray queried shadows on/off and a wireframe debug fill mode,
// all sharing one layout so they swap mid-frame without rebinding sets.
class MeshPipeline {
 public:
  // Third scene-pass attachment: diffuse-only lighting of skin materials plus
  // a mask, consumed by the screen-space subsurface scattering blur.
  static constexpr Format kSkinDiffuseFormat = Format::kRGBA16Float;

  // bindless_layout enables set 3 (the scene tables the rt variant reads for
  // reflection hit shading); pass a null handle when ray query is unavailable.
  // samples > 1 builds the opaque/prepass raster pipelines multisampled
  // (kMsaa mode). Blend pipelines stay single-sampled: the transparent pass
  // always runs after the resolve.
  static std::unique_ptr<MeshPipeline> Create(Device& device, Format color_format,
                                              Format motion_format, Format normal_format,
                                              Format depth_format,
                                              BindingLayoutHandle material_layout,
                                              BindingLayoutHandle environment_layout,
                                              BindingLayoutHandle bindless_layout,
                                              u32 samples = 1);
  ~MeshPipeline();

  MeshPipeline(const MeshPipeline&) = delete;
  MeshPipeline& operator=(const MeshPipeline&) = delete;

  BindingLayoutHandle set_layout() const { return set_layout_; }
  bool has_rt_variant() const { return static_cast<bool>(pipelines_[kRt]); }

  // use_rt selects the ray-query fragment variant (shadows and/or reflections);
  // bindless is bound as set 3 when the pipeline was built with it.
  void Bind(CommandList& cmd, BindingSetHandle globals, BindingSetHandle environment,
            BindingSetHandle bindless, bool use_rt, bool wireframe);
  void BindPrepass(CommandList& cmd, BindingSetHandle globals, BindingSetHandle environment);
  // Transparent variant: alpha blend over the opaque result, depth tested
  // against the prepass without writing. Set state mirrors Bind.
  void BindBlend(CommandList& cmd, BindingSetHandle globals, BindingSetHandle environment,
                 BindingSetHandle bindless, bool use_rt);
  // Additive-blend transparent variant (one, one): HDR effect-shader fire and
  // glows. Same shaders as BindBlend; the unlit branch premultiplies coverage.
  void BindBlendAdditive(CommandList& cmd, BindingSetHandle globals, BindingSetHandle environment,
                         BindingSetHandle bindless, bool use_rt);
  void BindMaterial(CommandList& cmd, BindingSetHandle material);
  void Draw(CommandList& cmd, const GpuMesh& mesh, const MeshPushConstants& push);
  void DrawSubmesh(CommandList& cmd, const GpuSubmesh& submesh);
  // Static streamed props use persistent current/previous matrix streams. The
  // instance permutation keeps the same material/frame sets and shaders, but
  // one indexed draw covers every placement in the group.
  void SetInstanced(CommandList& cmd, bool use_rt, bool wireframe);
  void SetInstancedPrepass(CommandList& cmd, bool masked);
  void DrawInstances(CommandList& cmd, const GpuMesh& mesh, const GpuBuffer& instances,
                     const GpuBuffer& previous_instances, const MeshPushConstants& push);

  // Optional mesh-shader opaque path. Built only when the device supports it;
  // the scene/prepass variants reuse the same descriptor sets and fragment
  // shaders as the raster path. BindMeshMaterial binds set 1 with the
  // mesh-shader layout; DrawMeshlets issues one workgroup per meshlet.
  bool has_mesh_shader() const {
    return static_cast<bool>(ms_scene_[0]) && static_cast<bool>(ms_prepass_);
  }
  void BindMeshScene(CommandList& cmd, BindingSetHandle globals, BindingSetHandle environment,
                     BindingSetHandle bindless, bool use_rt);
  void BindMeshPrepass(CommandList& cmd, BindingSetHandle globals);
  void BindMeshMaterial(CommandList& cmd, BindingSetHandle material);
  void DrawMeshlets(CommandList& cmd, const MeshShaderPush& push);

  // Swap the bound pipeline between the static and GPU-skinned vertex paths
  // mid-pass without rebinding descriptor sets (the layout is shared). The draw
  // loop calls these when it crosses a skinned/non-skinned mesh boundary.
  bool has_skinning() const { return static_cast<bool>(skinned_pipelines_[0]); }
  void SetSkinned(CommandList& cmd, bool skinned, bool use_rt, bool wireframe);
  // Binds the prepass pipeline for a submesh: skinned vs static vertex path,
  // masked (alpha-test discard) vs opaque fragment (keeps early-Z).
  void SetPrepassVariant(CommandList& cmd, bool skinned, bool masked);

 private:
  // Variant index bits.
  static constexpr u32 kRt = 1;
  static constexpr u32 kWire = 2;

  explicit MeshPipeline(Device& device) : device_(device) {}

  Device& device_;
  BindingLayoutHandle set_layout_;
  bool has_bindless_ = false;  // set 3 present in the layout
  PipelineHandle pipelines_[4] = {};  // [rt | wire]
  PipelineHandle blend_pipelines_[2] = {};  // [rt]
  PipelineHandle blend_additive_pipelines_[2] = {};  // [rt] additive effect vfx
  PipelineHandle prepass_pipeline_;
  PipelineHandle prepass_masked_pipeline_;  // alpha-test discard variant
  // Skinned vertex path: same fragment variants, extra vertex stream + VS.
  PipelineHandle skinned_pipelines_[2] = {};  // [rt]
  PipelineHandle skinned_prepass_pipeline_;
  PipelineHandle skinned_prepass_masked_pipeline_;
  PipelineHandle instanced_pipelines_[4] = {};  // [rt | wire]
  PipelineHandle instanced_prepass_pipeline_;
  PipelineHandle instanced_prepass_masked_pipeline_;
  // Optional mesh-shader opaque variants (larger mesh-stage push range).
  PipelineHandle ms_scene_[2] = {};  // [rt]
  PipelineHandle ms_prepass_;
};

}  // namespace rx::render

#endif  // RX_RENDER_MESH_PIPELINE_H_
