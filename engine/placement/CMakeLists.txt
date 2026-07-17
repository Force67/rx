# GPU-based procedural placement: WorldData maps + density programs compiled
# from ecotopes, discretized by ordered dithering. rx::placement is the
# CPU-side model + reference generator (headless-safe); rx::placement_gpu is
# the three-stage compute pipeline and needs the renderer.
rx_add_module(placement
  density_program.cc
  placement.cc
  world_data.cc)
target_link_libraries(rx_placement PUBLIC rx::core)

if(TARGET rx_render)
  rx_add_module(placement_gpu gpu_placement.cc)
  target_link_libraries(rx_placement_gpu PUBLIC rx::placement rx::render)
  # Quoted includes resolve against these (rhi_bindings.hlsli lives with the
  # render module); directory-scoped, so this does not leak to other modules.
  set(RX_SHADER_INCLUDE_DIRS
    ${PROJECT_SOURCE_DIR}/engine/render/shaders
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
  set(RX_SHADER_DEPS_placement_density_cs_hlsl shaders/placement_common.hlsli)
  set(RX_SHADER_DEPS_placement_generate_cs_hlsl shaders/placement_common.hlsli)
  set(RX_SHADER_DEPS_placement_transform_cs_hlsl shaders/placement_common.hlsli)
  rx_embed_shaders(rx_placement_gpu
    shaders/placement_density.cs.hlsl
    shaders/placement_generate.cs.hlsl
    shaders/placement_transform.cs.hlsl)
endif()
