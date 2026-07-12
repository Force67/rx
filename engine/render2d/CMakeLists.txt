# render2d: a reusable 2D rendering path (sprite batching, tilemaps, an
# orthographic 2D camera, isometric grid math and a small 2D deferred-lighting
# chain). It composites into rx's frame through the scene-opaque hook using its
# own ortho matrices, so 2D content still runs through rx's post chain (bloom,
# tonemap). Depends only on the RHI/renderer surface of rx::render.
rx_add_module(render2d
  tile_map.cc
  sprite_renderer.cc
)
target_link_libraries(rx_render2d PUBLIC rx::core rx::render)

# HLSL -> SPIR-V (dxc, SM 6.6, entry `main`), embedded as C arrays the module
# includes as shaders/<symbol>.h (see cmake/shaders.cmake).
rx_embed_shaders(rx_render2d
  shaders/sprite.vs.hlsl
  shaders/sprite.ps.hlsl
  shaders/light2d.vs.hlsl
  shaders/light2d.ps.hlsl
  shaders/composite.vs.hlsl
  shaders/composite.ps.hlsl
)
