rx_add_module(render
  core/renderer.cc
  core/presets.cc
  core/settings_ini.cc
  core/render_graph.cc
  core/dynamic_resolution.cc
  core/bindless.cc
  pipeline/mesh_pipeline.cc
  pipeline/material_system.cc
  pipeline/meshlet.cc
  pipeline/gpu_cull.cc
  gi/shadow.cc
  gi/local_shadows.cc
  gi/shadow_trace.cc
  gi/ddgi.cc
  gi/light_grid.cc
  gi/rcgi.cc
  gi/sdf_scene.cc
  gi/sdf_clipmap.cc
  gi/raytracing.cc
  gi/rt_instance_cull.cc
  gi/path_tracer.cc
  gi/recon_path_tracer.cc
  screenspace/ambient_occlusion.cc
  screenspace/ssao.cc
  screenspace/ssr.cc
  screenspace/ssgi.cc
  screenspace/reflection_trace.cc
  atmosphere/environment.cc
  atmosphere/volumetric_fog.cc
  atmosphere/froxel_fog.cc
  atmosphere/aerial_perspective.cc
  atmosphere/clouds.cc
  atmosphere/precipitation.cc
  atmosphere/precip_occlusion.cc
  atmosphere/precip_volume.cc
  atmosphere/lightning.cc
  atmosphere/surface_weather.cc
  post/post.cc
  post/bloom.cc
  post/exposure.cc
  post/taa.cc
  post/overdraw.cc
  post/ui_blur.cc
  post/motion_blur.cc
  post/vrs_rate.cc
  gi/restir_di.cc
  texturing/virtual_texture.cc
  pipeline/virtual_geometry.cc
  geometry/hair_groom.cc
  geometry/hair_strands.cc
  geometry/ocean_fft.cc
  geometry/imposters.cc
  geometry/instance_store.cc
  post/depth_of_field.cc
  post/upscaler.cc
  geometry/water.cc
  geometry/adaptive_water.cc
  geometry/water_field.cc
  geometry/fluid_sim.cc
  geometry/fluid_surface.cc
  geometry/shore_wetting.cc
  geometry/water_caustics.cc
  geometry/particles.cc
  geometry/particle_emitters.cc
  geometry/procedural_grass.cc
  geometry/fur.cc
  geometry/wboit.cc
  geometry/gaussian.cc
  util/exr_write.cc
  util/gpu_profiler.cc
  util/stb_impl.cc
  rhi/rhi.cc
  null/null_device.cc
)

# Graphics backends. Vulkan is the default everywhere it can run; d3d12 is the
# native windows/xbox path, and on Linux it runs natively over vkd3d
# (D3D12-on-Vulkan) for cross-backend validation. The null backend is always
# built.
option(RX_RHI_VULKAN "Build the Vulkan render backend" ON)
if(WIN32)
  option(RX_RHI_D3D12 "Build the D3D12 render backend" ON)
else()
  # Default to ON when a D3D12 provider is available (the nix dev shell
  # provides both WineHQ vkd3d and vkd3d-proton).
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(VKD3D_PROBE QUIET libvkd3d libvkd3d-utils)
    if(NOT VKD3D_PROBE_FOUND)
      pkg_check_modules(VKD3D_PROBE QUIET libvkd3d-proton-d3d12)
    endif()
  endif()
  if(VKD3D_PROBE_FOUND)
    option(RX_RHI_D3D12 "Build the D3D12 render backend" ON)
  else()
    option(RX_RHI_D3D12 "Build the D3D12 render backend" OFF)
  endif()
endif()

if(RX_RHI_VULKAN)
  target_sources(rx_render PRIVATE
    vulkan/vk_device.cc
    vulkan/vk_swapchain.cc
    vulkan/vk_command_list.cc
    vulkan/vk_convert.cc
    vulkan/vk_interop.cc
    vulkan/vma_impl.cc
    util/shader_util.cc
  )
  # PUBLIC: interop consumers (gui backend, thumbnailer, upscalers) gate their
  # Vulkan paths on this define.
  target_compile_definitions(rx_render PUBLIC RX_RHI_VULKAN)
endif()

if(RX_RHI_D3D12)
  target_sources(rx_render PRIVATE
    d3d12/d3d12_device.cc
    d3d12/d3d12_command_list.cc
    d3d12/d3d12_swapchain.cc
    d3d12/d3d12_convert.cc
  )
  # PUBLIC: RX_SHADER grows the DXIL sidecar reference under this define, so
  # every consumer of the embed headers must agree.
  target_compile_definitions(rx_render PUBLIC RX_RHI_D3D12)
  # Linux D3D12 provider: WineHQ vkd3d (default) or vkd3d-proton, Proton's
  # implementation, which exposes DXR 1.1 / mesh shaders / SM 6.6 and so runs
  # the backend's ray tracing paths natively.
  option(RX_VKD3D_PROTON "Use vkd3d-proton as the Linux D3D12 provider" OFF)
  if(WIN32)
    target_link_libraries(rx_render PRIVATE d3d12 dxgi)
  elseif(RX_VKD3D_PROTON)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(VKD3D_PROTON REQUIRED IMPORTED_TARGET libvkd3d-proton-d3d12)
    target_compile_definitions(rx_render PRIVATE RX_VKD3D_PROTON)
    target_link_libraries(rx_render PRIVATE $<BUILD_INTERFACE:PkgConfig::VKD3D_PROTON>)
    set(RX_INSTALL_RHI_D3D12 ON PARENT_SCOPE)
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(VKD3D REQUIRED IMPORTED_TARGET libvkd3d libvkd3d-utils)
    # BUILD_INTERFACE: kept out of the installed export; rxConfig.cmake
    # re-resolves vkd3d via pkg-config for find_package consumers.
    target_link_libraries(rx_render PRIVATE $<BUILD_INTERFACE:PkgConfig::VKD3D>)
    set(RX_INSTALL_RHI_D3D12 ON PARENT_SCOPE)
  endif()
endif()

# volk / VMA are PUBLIC (their headers propagate through the RHI interop
# surface). Wrapped in BUILD_INTERFACE so they stay out of the installed export;
# rxConfig.cmake re-adds bundled imported equivalents for find_package consumers.
# Under RX_SHARED volk is compiled hidden (see the top-level CMakeLists), so each
# DSO that links it gets a private, non-interposing copy it initializes itself.
target_link_libraries(rx_render
  PUBLIC rx::core rx::asset
         $<BUILD_INTERFACE:volk> $<BUILD_INTERFACE:GPUOpen::VulkanMemoryAllocator>)
target_link_libraries(rx_render PRIVATE $<BUILD_INTERFACE:rx::stb>)

if(RX_FSR3)
  target_sources(rx_render PRIVATE
    post/upscaler_fsr3.cc
    post/frame_generation.cc)
  target_compile_definitions(rx_render PRIVATE RX_HAS_FSR3)
  target_link_libraries(rx_render PRIVATE $<BUILD_INTERFACE:rx::ffx_fsr3>)
  set(RX_INSTALL_FSR3 ON PARENT_SCOPE)
endif()

if(RX_DLSS)
  target_sources(rx_render PRIVATE
    post/upscaler_dlss.cc
    post/ngx_context.cc
    gi/denoiser_rr.cc)
  # PUBLIC: renderer.h embeds an RrDenoiser member under this macro, so every
  # translation unit that includes it must agree on the layout (same rule as
  # RX_HAS_NRD below).
  target_compile_definitions(rx_render PUBLIC RX_HAS_DLSS)
  target_link_libraries(rx_render PRIVATE $<BUILD_INTERFACE:rx::dlss>)
  set(RX_INSTALL_DLSS ON PARENT_SCOPE)
endif()

if(RX_NRD)
  target_sources(rx_render PRIVATE gi/denoiser_nrd.cc)
  # PUBLIC: renderer.h embeds an NrdDenoiser member under this macro, so every
  # translation unit that includes it must agree on the layout or the Renderer
  # ctor writes past a shorter allocation.
  target_compile_definitions(rx_render PUBLIC RX_HAS_NRD)
  target_link_libraries(rx_render PRIVATE $<BUILD_INTERFACE:rx::nrd>)
  set(RX_INSTALL_NRD ON PARENT_SCOPE)
endif()

if(NOT MSVC)
  set_source_files_properties(vulkan/vma_impl.cc util/stb_impl.cc PROPERTIES COMPILE_OPTIONS -w)
endif()

set(RX_RENDER_SHADERS
  shaders/util/debug_line.vs.hlsl
  shaders/util/debug_line.ps.hlsl
  shaders/util/pick_id.vs.hlsl
  shaders/util/pick_id.ps.hlsl
  shaders/pipeline/mesh.vs.hlsl
  shaders/pipeline/mesh_instance.vs.hlsl
  shaders/pipeline/mesh_skin.vs.hlsl
  shaders/gi/shadow.vs.hlsl
  shaders/gi/shadow_instance.vs.hlsl
  shaders/gi/shadow_skin.vs.hlsl
  shaders/gi/shadow.ps.hlsl
  shaders/geometry/particle.vs.hlsl
  shaders/geometry/particle.ps.hlsl
  shaders/geometry/particle_tex.ps.hlsl
  shaders/geometry/particle_sim.cs.hlsl
  shaders/geometry/procedural_grass_bend.cs.hlsl
  shaders/geometry/procedural_grass_generate.cs.hlsl
  shaders/geometry/procedural_grass.vs.hlsl
  shaders/geometry/procedural_grass_prepass.ps.hlsl
  shaders/geometry/procedural_grass.ps.hlsl
  shaders/geometry/fur.vs.hlsl
  shaders/geometry/fur.ps.hlsl
  shaders/geometry/wboit.vs.hlsl
  shaders/geometry/wboit.ps.hlsl
  shaders/geometry/wboit_resolve.ps.hlsl
  shaders/geometry/gsplat.vs.hlsl
  shaders/geometry/gsplat.ps.hlsl
  shaders/post/overdraw.ps.hlsl
  shaders/pipeline/mesh.ps.hlsl
  shaders/pipeline/mesh_rt.ps.hlsl
  shaders/pipeline/mesh_sss.ps.hlsl
  shaders/pipeline/mesh_rt_sss.ps.hlsl
  shaders/util/fullscreen.vs.hlsl
  shaders/util/blit.ps.hlsl
  shaders/util/msaa_resolve.cs.hlsl
  shaders/util/depth_copy.ps.hlsl
  shaders/post/tonemap.ps.hlsl
  shaders/post/taa.cs.hlsl
  shaders/atmosphere/transmittance_lut.cs.hlsl
  shaders/atmosphere/multiscatter_lut.cs.hlsl
  shaders/atmosphere/aerial_perspective.cs.hlsl
  shaders/atmosphere/clouds.cs.hlsl
  shaders/atmosphere/cloud_shadow.cs.hlsl
  shaders/atmosphere/froxel_scatter.cs.hlsl
  shaders/atmosphere/froxel_integrate.cs.hlsl
  shaders/atmosphere/froxel_apply.cs.hlsl
  shaders/atmosphere/precipitation.cs.hlsl
  shaders/atmosphere/precip_volume.vs.hlsl
  shaders/atmosphere/precip_volume_rt.vs.hlsl
  shaders/atmosphere/precip_volume.ps.hlsl
  shaders/atmosphere/precip_splash.vs.hlsl
  shaders/atmosphere/precip_splash.ps.hlsl
  shaders/atmosphere/lightning_bolt.vs.hlsl
  shaders/atmosphere/lightning_bolt.ps.hlsl
  shaders/atmosphere/surface_weather.cs.hlsl
  shaders/atmosphere/sky.cs.hlsl
  shaders/atmosphere/sky.ps.hlsl
  shaders/atmosphere/irradiance.cs.hlsl
  shaders/atmosphere/prefilter.cs.hlsl
  shaders/atmosphere/brdf_lut.cs.hlsl
  shaders/pipeline/prepass.ps.hlsl
  shaders/pipeline/prepass_masked.ps.hlsl
  shaders/screenspace/rtao.cs.hlsl
  shaders/screenspace/ssao.cs.hlsl
  shaders/screenspace/ssr.cs.hlsl
  shaders/screenspace/ssgi.cs.hlsl
  shaders/screenspace/reflection_trace.cs.hlsl
  shaders/screenspace/reflection_upscale.cs.hlsl
  shaders/screenspace/contact_shadow.cs.hlsl
  shaders/screenspace/sss_blur.cs.hlsl
  shaders/pipeline/cull.cs.hlsl
  shaders/pipeline/light_cluster.cs.hlsl
  shaders/pipeline/hiz_reduce.cs.hlsl
  shaders/pipeline/meshlet.ms.hlsl
  shaders/pipeline/meshlet.ps.hlsl
  shaders/pipeline/mesh_scene.as.hlsl
  shaders/pipeline/mesh_scene.ms.hlsl
  shaders/pipeline/bounds.vs.hlsl
  shaders/pipeline/bounds.ps.hlsl
  shaders/post/bloom_down.cs.hlsl
  shaders/post/bloom_up.cs.hlsl
  shaders/post/histogram.cs.hlsl
  shaders/post/exposure_resolve.cs.hlsl
  shaders/post/vrs_rate.cs.hlsl
  shaders/gi/restir_di_temporal.cs.hlsl
  shaders/gi/restir_di_spatial.cs.hlsl
  shaders/pipeline/vgeo.ms.hlsl
  shaders/pipeline/vgeo_cull.cs.hlsl
  shaders/pipeline/vgeo_args.cs.hlsl
  shaders/pipeline/vgeo_clear.cs.hlsl
  shaders/pipeline/vgeo_sw.cs.hlsl
  shaders/pipeline/vgeo_hzb.cs.hlsl
  shaders/pipeline/vgeo_vis.ms.hlsl
  shaders/pipeline/vgeo_vis.ps.hlsl
  shaders/pipeline/vgeo_resolve.ps.hlsl
  shaders/geometry/hair.vs.hlsl
  shaders/geometry/hair.ps.hlsl
  shaders/geometry/ocean_spectrum.cs.hlsl
  shaders/geometry/ocean_fft.cs.hlsl
  shaders/geometry/ocean_finalize.cs.hlsl
  shaders/geometry/ocean_normals.cs.hlsl
  shaders/geometry/imposter_bake.vs.hlsl
  shaders/geometry/imposter_bake.ps.hlsl
  shaders/geometry/imposter.vs.hlsl
  shaders/geometry/imposter.ps.hlsl
  shaders/gi/ddgi_rays.cs.hlsl
  shaders/gi/ddgi_blend.cs.hlsl
  shaders/gi/ddgi_border.cs.hlsl
  shaders/gi/light_grid.cs.hlsl
  shaders/gi/rcgi_probe_trace.cs.hlsl
  shaders/gi/rcgi_probe_trace_sw.cs.hlsl
  shaders/gi/rcgi_args.cs.hlsl
  shaders/gi/rcgi_cache_shade.cs.hlsl
  shaders/gi/rcgi_cache_shade_sw.cs.hlsl
  shaders/gi/rcgi_blend.cs.hlsl
  shaders/gi/rcgi_border.cs.hlsl
  shaders/gi/rcgi_probe_meta.cs.hlsl
  shaders/gi/rcgi_resolve.cs.hlsl
  shaders/gi/rcgi_gather.cs.hlsl
  shaders/gi/rcgi_denoise.cs.hlsl
  shaders/gi/rcgi_upscale.cs.hlsl
  shaders/gi/rcgi_history.cs.hlsl
  shaders/gi/sdf_clear.cs.hlsl
  shaders/gi/sdf_compose.cs.hlsl
  shaders/gi/sdf_debug.cs.hlsl
  shaders/geometry/water.ps.hlsl
  shaders/geometry/adaptive_water.cs.hlsl
  shaders/geometry/water_field.cs.hlsl
  shaders/geometry/fluid_sim.cs.hlsl
  shaders/geometry/fluid_surface.vs.hlsl
  shaders/geometry/fluid_surface.ps.hlsl
  shaders/geometry/shore_wetting.cs.hlsl
  shaders/geometry/water_caustics.cs.hlsl
  shaders/geometry/copy.cs.hlsl
  shaders/util/hdr_capture.cs.hlsl
  shaders/gi/pathtrace.cs.hlsl
  shaders/gi/recon_gbuffer.cs.hlsl
  shaders/gi/recon_restir_temporal.cs.hlsl
  shaders/gi/recon_restir_spatial.cs.hlsl
  shaders/gi/recon_fog.cs.hlsl
  shaders/gi/recon_sky_cdf.cs.hlsl
  shaders/gi/recon_restir_di_temporal.cs.hlsl
  shaders/gi/recon_restir_di_spatial.cs.hlsl
  shaders/gi/recon_temporal.cs.hlsl
  shaders/gi/recon_atrous.cs.hlsl
  shaders/gi/recon_composite.cs.hlsl
  shaders/atmosphere/fog.cs.hlsl
  shaders/post/ui_blur.ps.hlsl
  shaders/post/motion_tilemax.cs.hlsl
  shaders/post/motion_blur.cs.hlsl
  shaders/post/dof_coc.cs.hlsl
  shaders/post/dof_gather.cs.hlsl
  shaders/post/dof_composite.cs.hlsl
)

# rtao.cs.hlsl and the nrd_* shaders pull in NRD.hlsli. The include dir is added
# whenever NRD is vendored (independent of RX_NRD, so an NRD-off build of
# a vendored tree still compiles rtao). The denoiser-only inputs are embedded
# only when the denoiser is built.
if(EXISTS ${PROJECT_SOURCE_DIR}/third_party/NRD/Shaders/NRD.hlsli)
  set(RX_SHADER_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/third_party/NRD/Shaders)
endif()
# Shaders include siblings from their own subfolder (atmosphere.hlsli; the
# mesh/shadow base vertex shaders reused by the *_skin variants). Add those dirs
# so dxc resolves the quoted includes regardless of the including file's path.
list(APPEND RX_SHADER_INCLUDE_DIRS
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/atmosphere
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/geometry
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/pipeline
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gi)
if(RX_NRD)
  list(APPEND RX_RENDER_SHADERS shaders/gi/nrd_pack.cs.hlsl shaders/gi/shadow_trace.cs.hlsl
       shaders/gi/pathtrace_gbuffer.cs.hlsl shaders/gi/pathtrace_composite.cs.hlsl)
endif()

# SPIR-V-only shaders: what's left of the vk::RawBufferLoad (buffer device
# address) users. The ray-hit shading readers now go through the dual-path
# helpers in shaders/rt_geometry.hlsli (BDA on SPIR-V, the bindless
# ByteAddressBuffer array on DXIL) and compile for d3d12; only the mesh-shader
# path remains: mesh_scene reads meshlet data through push-constant addresses
# and meshlet.ms trips DXIL validation (SetMeshOutputCounts in two branches).
# The skinned vertex permutations carry their own dual path
# (rec_bone_palette root SRV on DXIL).
# Wrapper-shader include dependencies (see cmake/shaders.cmake).
set(RX_SHADER_DEPS_mesh_vs_hlsl shaders/pipeline/morph.hlsli)
set(RX_SHADER_DEPS_sky_ps_hlsl shaders/atmosphere/aurora.hlsli)
set(RX_SHADER_DEPS_sky_cs_hlsl shaders/atmosphere/aurora.hlsli)
set(RX_SHADER_DEPS_mesh_instance_vs_hlsl shaders/pipeline/mesh.vs.hlsl shaders/pipeline/morph.hlsli)
set(RX_SHADER_DEPS_mesh_skin_vs_hlsl shaders/pipeline/mesh.vs.hlsl shaders/pipeline/morph.hlsli)
set(RX_SHADER_DEPS_mesh_sss_ps_hlsl shaders/pipeline/mesh.ps.hlsl)
set(RX_SHADER_DEPS_mesh_rt_ps_hlsl shaders/gi/rcgi_common.hlsli)
set(RX_SHADER_DEPS_prepass_masked_ps_hlsl shaders/pipeline/prepass.ps.hlsl)
set(RX_SHADER_DEPS_shadow_instance_vs_hlsl shaders/gi/shadow.vs.hlsl)
set(RX_SHADER_DEPS_mesh_rt_sss_ps_hlsl shaders/pipeline/mesh_rt.ps.hlsl shaders/gi/rcgi_common.hlsli)
set(RX_SHADER_DEPS_vgeo_cull_cs_hlsl shaders/pipeline/vgeo_common.hlsli)
set(RX_SHADER_DEPS_vgeo_args_cs_hlsl shaders/pipeline/vgeo_common.hlsli)
set(RX_SHADER_DEPS_vgeo_clear_cs_hlsl shaders/pipeline/vgeo_common.hlsli)
set(RX_SHADER_DEPS_vgeo_sw_cs_hlsl shaders/pipeline/vgeo_common.hlsli)
set(RX_SHADER_DEPS_vgeo_hzb_cs_hlsl shaders/pipeline/vgeo_common.hlsli)
set(RX_SHADER_DEPS_vgeo_vis_ms_hlsl shaders/pipeline/vgeo_common.hlsli)
set(RX_SHADER_DEPS_vgeo_vis_ps_hlsl shaders/pipeline/vgeo_common.hlsli)
set(RX_SHADER_DEPS_vgeo_resolve_ps_hlsl shaders/pipeline/vgeo_common.hlsli)
set(RX_SHADER_DEPS_particle_tex_ps_hlsl shaders/geometry/particle.ps.hlsl)
set(RX_SHADER_DEPS_procedural_grass_vs_hlsl shaders/geometry/procedural_grass.hlsli)
set(RX_SHADER_DEPS_procedural_grass_prepass_ps_hlsl shaders/geometry/procedural_grass.hlsli shaders/gi/material_class.hlsli)
set(RX_SHADER_DEPS_procedural_grass_ps_hlsl shaders/geometry/procedural_grass.hlsli)
set(RX_SHADER_DEPS_precip_volume_vs_hlsl shaders/atmosphere/precip_common.hlsli)
set(RX_SHADER_DEPS_precip_volume_rt_vs_hlsl shaders/atmosphere/precip_volume.vs.hlsl shaders/atmosphere/precip_common.hlsli)
set(RX_SHADER_DEPS_precip_volume_ps_hlsl shaders/atmosphere/precip_common.hlsli)
set(RX_SHADER_DEPS_precip_splash_vs_hlsl shaders/atmosphere/precip_common.hlsli)
set(RX_SHADER_DEPS_precip_splash_ps_hlsl shaders/atmosphere/precip_common.hlsli)
set(RX_SHADER_DEPS_lightning_bolt_vs_hlsl shaders/atmosphere/lightning_common.hlsli shaders/atmosphere/precip_common.hlsli)
set(RX_SHADER_DEPS_lightning_bolt_ps_hlsl shaders/atmosphere/lightning_common.hlsli shaders/atmosphere/precip_common.hlsli)
set(RX_SHADER_DEPS_light_grid_cs_hlsl shaders/gi/light_grid.hlsli)
set(RX_SHADER_DEPS_rcgi_probe_trace_cs_hlsl shaders/gi/rcgi_probe_trace_body.hlsli shaders/gi/rcgi_common.hlsli shaders/rt_geometry.hlsli)
set(RX_SHADER_DEPS_rcgi_probe_trace_sw_cs_hlsl shaders/gi/rcgi_probe_trace_body.hlsli shaders/gi/rcgi_common.hlsli shaders/gi/sdf_trace.hlsli)
set(RX_SHADER_DEPS_rcgi_cache_shade_cs_hlsl shaders/gi/rcgi_cache_shade_body.hlsli shaders/gi/rcgi_common.hlsli shaders/gi/light_grid.hlsli shaders/rt_geometry.hlsli)
set(RX_SHADER_DEPS_rcgi_cache_shade_sw_cs_hlsl shaders/gi/rcgi_cache_shade_body.hlsli shaders/gi/rcgi_common.hlsli shaders/gi/light_grid.hlsli shaders/gi/sdf_trace.hlsli)
set(RX_SHADER_DEPS_rcgi_blend_cs_hlsl shaders/gi/rcgi_common.hlsli)
set(RX_SHADER_DEPS_rcgi_probe_meta_cs_hlsl shaders/gi/rcgi_common.hlsli)
set(RX_SHADER_DEPS_rcgi_resolve_cs_hlsl shaders/gi/rcgi_common.hlsli)
set(RX_SHADER_DEPS_rcgi_gather_cs_hlsl shaders/gi/rcgi_common.hlsli shaders/gi/sh.hlsli)
set(RX_SHADER_DEPS_rcgi_denoise_cs_hlsl shaders/gi/rcgi_common.hlsli)
set(RX_SHADER_DEPS_rcgi_upscale_cs_hlsl shaders/gi/rcgi_common.hlsli shaders/gi/sh.hlsli)
set(RX_SHADER_DEPS_reflection_trace_cs_hlsl shaders/rt_geometry.hlsli shaders/gi/sh.hlsli shaders/gi/rcgi_common.hlsli)
set(RX_SHADER_DEPS_sdf_clear_cs_hlsl shaders/gi/sdf_trace.hlsli)
set(RX_SHADER_DEPS_sdf_compose_cs_hlsl shaders/gi/sdf_trace.hlsli)
set(RX_SHADER_DEPS_sdf_debug_cs_hlsl shaders/gi/sdf_trace.hlsli)

# The gpu-driven virtual geometry shaders touching the 64-bit visibility
# buffer need SM 6.6 atomics/types the 6.5 DXIL sidecar cannot express; d3d12
# runs the single-pass fallback instead (caps().buffer_atomics64 stays false).
set(RX_SHADER_NO_DXIL
  mesh_scene.as.hlsl
  mesh_scene.ms.hlsl
  meshlet.ms.hlsl
  vgeo_clear.cs.hlsl
  vgeo_sw.cs.hlsl
  vgeo_hzb.cs.hlsl
  vgeo_vis.ms.hlsl
  vgeo_vis.ps.hlsl
  vgeo_resolve.ps.hlsl
)

# Optional RHI imgui render backend. engine/render must not hard-depend on
# imgui, so this compiles only when the vendored imgui core target exists. It
# links imgui core only (no SDL3, no raw-Vulkan backend, no second volk copy);
# the shaders embed alongside the rest of the render module's.
if(TARGET rx_imgui_core)
  target_sources(rx_render PRIVATE util/imgui_renderer.cc)
  # BUILD_INTERFACE: kept out of the installed export (find_package consumers
  # bring their own imgui); add_subdirectory consumers link it directly.
  target_link_libraries(rx_render PRIVATE $<BUILD_INTERFACE:rx_imgui_core>)
  list(APPEND RX_RENDER_SHADERS
    shaders/util/imgui.vs.hlsl
    shaders/util/imgui.ps.hlsl)
endif()

rx_embed_shaders(rx_render ${RX_RENDER_SHADERS})
