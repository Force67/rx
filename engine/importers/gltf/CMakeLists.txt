rx_add_module(gltf gltf_loader.cc)
target_link_libraries(rx_gltf PUBLIC rx::asset rx::core)
# Header-only, PRIVATE: only needed to compile the loader. BUILD_INTERFACE keeps
# them out of the installed export.
target_link_libraries(rx_gltf PRIVATE
  $<BUILD_INTERFACE:rx::cgltf> $<BUILD_INTERFACE:rx::stb_impl>)
if(NOT MSVC)
  # vendored single header libraries are not warning clean
  set_source_files_properties(gltf_loader.cc PROPERTIES COMPILE_OPTIONS -Wno-shadow)
endif()
