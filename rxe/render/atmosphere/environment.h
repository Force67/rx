#ifndef RX_RENDER_ENVIRONMENT_H_
#define RX_RENDER_ENVIRONMENT_H_


#include "base/memory/unique_pointer.h"
#include "foundation/math/math.h"
#include "rxe/gpu/rhi/device.h"

namespace rx::render {

// Image based lighting around a procedural atmosphere. Owns the sky cubemap
// plus its diffuse irradiance and ggx prefiltered convolutions, the split
// sum brdf lut, and the sky background pipeline. The cubemaps regenerate in
// frame whenever the sun changes; the lut bakes once at startup.
//
// Also owns the layout of descriptor set 2 of the mesh pipeline (ibl inputs,
// the per frame ao texture and the ddgi probe volume) and neutral fallbacks
// for whatever feature is disabled.
class EnvironmentSystem {
 public:
  static constexpr u32 kSkySize = 128;
  static constexpr u32 kIrradianceSize = 32;
  static constexpr u32 kPrefilterSize = 128;
  static constexpr u32 kPrefilterMips = 6;  // matches kPrefilterMips in mesh.ps
  static constexpr u32 kBrdfLutSize = 512;
  // Hillaire 2020 atmosphere LUTs (sun-independent, baked once).
  static constexpr u32 kTransmittanceW = 256;
  static constexpr u32 kTransmittanceH = 64;
  static constexpr u32 kMultiScatterSize = 32;

  static base::UniquePointer<EnvironmentSystem> Create(gpu::Device& device);
  ~EnvironmentSystem();

  // Builds the sky background pipeline; needs the mesh pipeline's set 0
  // layout and the scene pass attachment formats, so it runs after the mesh
  // pipeline exists.
  bool CreateSkyPipeline(gpu::BindingLayoutHandle globals_layout, gpu::Format color_format,
                         gpu::Format motion_format, gpu::Format depth_format);

  EnvironmentSystem(const EnvironmentSystem&) = delete;
  EnvironmentSystem& operator=(const EnvironmentSystem&) = delete;

  // Re-renders the sky and both convolutions with internal barriers. Call
  // from the frame command list when the sun changed, or on the aurora's
  // animation step while a night display is active (aurora_intensity > 0
  // bakes the curtains into the cubemap so they light the world through the
  // convolutions; pass 0 to skip them).
  void RecordUpdate(gpu::CommandList& cmd, const Vec3& sun_direction, f32 sun_intensity,
                    const Vec3& sun_color, f32 aurora_intensity, f32 time_seconds);

  // Replaces the procedural atmosphere with an authored equirectangular HDR as
  // the source of the sky cubemap, and therefore of the IBL convolutions. This
  // is what a UsdLux DomeLight is: a scene's own sky, which the procedural
  // model cannot reproduce (the Attic's is a photographed environment). `rgba`
  // is `width * height` linear float4 texels, row major, +y at the top row.
  // Returns false if the upload fails; the procedural sky then stays in use.
  bool SetEnvironmentMap(const f32* rgba, u32 width, u32 height, const Vec3& tint,
                         f32 intensity, f32 rotation_radians);
  void ClearEnvironmentMap();
  bool has_environment_map() const { return has_envmap_; }

  // Fullscreen sky at the far plane; call inside the scene rendering pass.
  void DrawSky(gpu::CommandList& cmd, gpu::BindingSetHandle globals);

  gpu::BindingLayoutHandle env_set_layout() const { return env_set_layout_; }
  gpu::TextureView sky_view() const { return sky_.view; }
  gpu::TextureView transmittance_view() const { return transmittance_lut_.view; }
  gpu::TextureView multiscatter_view() const { return multiscatter_lut_.view; }
  gpu::SamplerHandle sampler() const { return sampler_; }
  gpu::TextureView prefiltered_view() const { return prefiltered_.view; }
  // Neutral stand-ins for passes that statically bind ddgi inputs.
  gpu::TextureView black_array_view() const { return black_array_view_; }
  const gpu::GpuBuffer& dummy_volume() const { return dummy_volume_; }
  const gpu::GpuBuffer& dummy_storage() const { return dummy_storage_; }
  // 1x1 black Texture2D (shader-read) for passes that statically bind a 2D
  // texture slot they may not use this frame (e.g. the RCGI cascade atlases in
  // reflection_trace when RCGI is off).
  gpu::TextureView black_view() const { return black_.view; }
  gpu::TextureView shadow_dummy_view() const { return shadow_dummy_.view; }
  gpu::SamplerHandle comparison_sampler() const { return shadow_sampler_; }

  struct DdgiBinding {
    gpu::TextureView irradiance;
    gpu::TextureView distance;
    gpu::GpuBuffer volume;
    u64 volume_size = 0;
    // Kept for callers that tracked the atlas layout; the RHI backend derives
    // image layouts from the binding type, so this is no longer consumed.
    gpu::ResourceState layout = gpu::ResourceState::kShaderReadFragment;
  };

  // RCGI world irradiance cascades for the inline reflection bounce (env slots
  // 36-40). Null -> placeholders (kFrameRcgi stays clear, never sampled). The
  // atlases are storage-written each frame and stay in GENERAL.
  struct RcgiWorldBinding {
    gpu::TextureView irradiance;
    gpu::TextureView visibility;
    const gpu::GpuBuffer* globals = nullptr;
    const gpu::GpuBuffer* probe_meta = nullptr;
    const gpu::GpuBuffer* interior_vols = nullptr;
  };

  // The hair transmittance volume (HairStrands), so the skin UNDER a groom is
  // shadowed by the fibres over it. Hair that casts nothing on the scalp is the
  // most visible thing wrong with a rendered head, and a binary shadow map
  // cannot supply it - hair is not opaque.
  struct HairVolumeBinding {
    gpu::TextureView front_depth;
    gpu::TextureView layers;
    const gpu::GpuBuffer* params = nullptr;
  };

  // Fills a freshly allocated set 2. Null ao view, ddgi binding, shadow view or
  // sun-shadow view fall back to the neutral dummies (white ao, black ddgi, lit
  // cascade shadow, fully-lit sun shadow).
  void WriteEnvSet(gpu::BindingSetHandle set, gpu::TextureView ao_view, const DdgiBinding* ddgi,
                   gpu::TextureView shadow_view = {},
                   const gpu::GpuBuffer& cascade_buffer = {}, u64 cascade_size = 0,
                   gpu::TextureView opaque_color = {},
                   gpu::TextureView sun_shadow_view = {},
                   const gpu::GpuBuffer& lights = {}, u64 lights_size = 0,
                   gpu::TextureView spec_reflections = {},
                   const gpu::GpuBuffer& cluster_counts = {},
                   const gpu::GpuBuffer& cluster_indices = {},
                   const gpu::GpuBuffer& decal_buffer = {},
                   const gpu::GpuBuffer& decal_indices = {},
                   gpu::TextureView decal_atlas = {},
                   const gpu::GpuBuffer& local_shadow_faces = {},
                   gpu::TextureView local_shadow_atlas = {},
                   gpu::TextureView decal_normal_atlas = {},
                   gpu::TextureView restir_diffuse = {}, gpu::TextureView restir_spec = {},
                   const gpu::GpuBuffer& vt_feedback = {}, gpu::TextureView vt_indirection = {},
                   gpu::TextureView vt_atlas = {}, gpu::TextureView ocean_displacement = {},
                   gpu::TextureView ocean_normal = {}, gpu::TextureView water_field_ring0 = {},
                   gpu::TextureView water_field_ring1 = {},
                   const gpu::GpuBuffer& water_field_params = {},
                   gpu::TextureView shore_wetness = {},
                   gpu::TextureView caustics = {},
                   gpu::TextureView rcgi_irradiance = {},
                   const RcgiWorldBinding* rcgi_world = nullptr,
                   gpu::TextureView decal_layer_albedo = {},
                   gpu::TextureView decal_layer_fx = {},
                   const gpu::GpuBuffer& decal_layer_xform = {},
                   const HairVolumeBinding* hair_volume = nullptr) const;

 private:
  explicit EnvironmentSystem(gpu::Device& device) : device_(device) {}

  void RecordConvolutions(gpu::CommandList& cmd, gpu::ResourceState conv_old);

  bool CreatePipelines();
  bool CreateImages();
  bool CreateDummies();
  bool BakeBrdfLut();
  bool BakeLuts();  // transmittance + multiple-scattering, baked once

  gpu::Device& device_;
  gpu::SamplerHandle sampler_;
  gpu::SamplerHandle shadow_sampler_;  // comparison sampler for cascades

  gpu::GpuImage sky_;         // rgba16f cube
  // Authored sky: an equirect HDR that replaces the procedural atmosphere as
  // the cubemap's source (see SetEnvironmentMap).
  gpu::GpuImage envmap_;
  bool has_envmap_ = false;
  Vec3 envmap_tint_{1.0f, 1.0f, 1.0f};
  f32 envmap_intensity_ = 1.0f;
  f32 envmap_rotation_ = 0.0f;
  gpu::GpuImage irradiance_;  // rgba16f cube
  gpu::GpuImage prefiltered_; // rgba16f cube, kPrefilterMips
  gpu::GpuImage brdf_lut_;    // rg16f
  gpu::GpuImage transmittance_lut_;  // rgba16f 2d, Hillaire transmittance
  gpu::GpuImage multiscatter_lut_;   // rgba16f 2d, Hillaire multiple scattering
  gpu::TextureView sky_storage_view_;          // mip 0 storage view
  gpu::TextureView irradiance_storage_view_;   // mip 0 storage view
  gpu::TextureView prefilter_storage_views_[kPrefilterMips] = {};

  // Neutral fallbacks: white ao, black ddgi atlases, lit shadow, zeroed volume.
  gpu::GpuImage white_;
  gpu::GpuImage black_array_;
  gpu::GpuImage shadow_dummy_;  // 1x1 depth cleared to 1.0 (fully lit)
  gpu::GpuImage flat_normal_;   // 1x1 (0.5, 0.5, 1) for the decal channel atlas
  gpu::GpuImage black_;         // 1x1 zero, restir-di / vt dummies
  gpu::SamplerHandle point_sampler_;  // nearest+mips, virtual-texture indirection
  gpu::SamplerHandle wrap_sampler_;   // repeat, fft-ocean tiles
  // Equirect env maps: longitude is periodic and must repeat or a seam
  // shows where u crosses 0/1; latitude clamps at the poles.
  gpu::SamplerHandle envmap_sampler_;
  // LTC fit tables for GGX area lights (64x64 RGBA16F, uploaded once).
  gpu::GpuImage ltc_matrix_;
  gpu::GpuImage ltc_amplitude_;
  gpu::TextureView black_array_view_;
  gpu::GpuBuffer dummy_volume_;
  gpu::GpuBuffer dummy_storage_;  // storage-usage fallback for the SB slots

  gpu::PipelineHandle sky_gen_;
  gpu::PipelineHandle envmap_gen_;
  gpu::PipelineHandle irradiance_gen_;
  gpu::PipelineHandle prefilter_gen_;
  gpu::PipelineHandle brdf_gen_;
  gpu::PipelineHandle transmittance_gen_;
  gpu::PipelineHandle multiscatter_gen_;

  gpu::BindingLayoutHandle env_set_layout_;
  gpu::PipelineHandle sky_draw_pipeline_;

  bool maps_initialized_ = false;  // first update transitions from kUndefined
};

}  // namespace rx::render

#endif  // RX_RENDER_ENVIRONMENT_H_
