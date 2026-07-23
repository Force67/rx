# render2d: a reusable 2D rendering path (sprite batching, tilemaps, an
# orthographic 2D camera, isometric grid math and a small 2D deferred-lighting
# chain). It composites into rx's resolved HDR overlay using its own ortho
# matrix, after temporal effects but before exposure, bloom and tonemapping.
# Depends only on the RHI/renderer surface of rx::render.
rx_add_module(render2d
  tile_map.cc
  sprite_renderer.cc
)
target_link_libraries(rx_render2d PUBLIC rx::core rx::render)

# HLSL -> SPIR-V (dxc, SM 6.6, entry `main`), embedded as C arrays the module
# includes as shaders/<symbol>.h (see cmake/shaders.cmake).
set(RX_SHADER_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/engine/render/shaders)
set(RX_SHADER_DEPS_sprite_vs_hlsl ../render/shaders/rhi_bindings.hlsli)
set(RX_SHADER_DEPS_light2d_vs_hlsl ../render/shaders/rhi_bindings.hlsli)
rx_embed_shaders(rx_render2d
  shaders/sprite.vs.hlsl
  shaders/sprite.ps.hlsl
  shaders/light2d.vs.hlsl
  shaders/light2d.ps.hlsl
  shaders/composite.vs.hlsl
  shaders/composite.ps.hlsl
)
