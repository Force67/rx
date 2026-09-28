# rx::imgui_renderer: draws Dear ImGui's draw lists through the RHI, with the rx
# theme. Built only when the vendored imgui core exists, so nothing else has to
# depend on imgui.
if(TARGET rx_imgui_core)
  rx_add_module(imgui_renderer imgui_renderer.cc)
  target_link_libraries(rx_imgui_renderer PUBLIC rx::gpu rx::foundation)
  # BUILD_INTERFACE: kept out of the installed export (find_package consumers
  # bring their own imgui); add_subdirectory consumers link it directly.
  target_link_libraries(rx_imgui_renderer PRIVATE $<BUILD_INTERFACE:rx_imgui_core>)
  set(RX_SHADER_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/rxe/render/shaders)
  rx_embed_shaders(rx_imgui_renderer
    shaders/imgui.vs.hlsl
    shaders/imgui.ps.hlsl)
  unset(RX_SHADER_INCLUDE_DIRS)
endif()
