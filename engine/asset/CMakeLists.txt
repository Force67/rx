rx_add_module(asset
  vfs.cc
  asset_database.cc
  primitives.cc
  simplify.cc
  subdivide.cc
  gltf_loader.cc
  materialx.cc
)

target_link_libraries(rx_asset PUBLIC rx::core)
target_link_libraries(rx_asset PRIVATE rx::cgltf rx::stb)

if(NOT MSVC)
  # vendored single header libraries are not warning clean
  set_source_files_properties(gltf_loader.cc PROPERTIES COMPILE_OPTIONS -Wno-shadow)
endif()
