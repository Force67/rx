rx_add_module(gltf gltf_loader.cc)
target_link_libraries(rx_gltf PUBLIC rx::asset rx::foundation)
# Header-only, PRIVATE: only needed to compile the loader. BUILD_INTERFACE keeps
# them out of the installed export.
target_link_libraries(rx_gltf PRIVATE
  $<BUILD_INTERFACE:rx::cgltf> $<BUILD_INTERFACE:rx::stb_impl>)
if(NOT MSVC)
  # vendored single header libraries are not warning clean
  set_source_files_properties(gltf_loader.cc PROPERTIES COMPILE_OPTIONS -Wno-shadow)
endif()

if(RX_BUILD_TESTS)
  add_executable(gltf_skin_test gltf_skin_test.cc)
  target_link_libraries(gltf_skin_test PRIVATE rx::gltf rx::asset rx::foundation)
  add_test(NAME gltf_skin_test COMMAND gltf_skin_test)

  add_executable(gltf_texture_transform_test gltf_texture_transform_test.cc)
  target_link_libraries(gltf_texture_transform_test PRIVATE rx::gltf rx::asset rx::foundation)
  add_test(NAME gltf_texture_transform_test COMMAND gltf_texture_transform_test)
endif()
