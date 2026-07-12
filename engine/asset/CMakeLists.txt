rx_add_module(asset
  vfs.cc
  pack.cc
  asset_database.cc
  primitives.cc
  simplify.cc
  subdivide.cc
  gltf_loader.cc
  materialx.cc
  # miniz compiles straight into the module (hidden visibility keeps its
  # symbols internal); no separate archive to bundle for installed consumers.
  ${PROJECT_SOURCE_DIR}/third_party/miniz/miniz.c
)

target_link_libraries(rx_asset PUBLIC rx::core)
# Header-only, PRIVATE: only needed to compile rx_asset itself. Wrapped in
# BUILD_INTERFACE so they never reach the installed export (find_package
# consumers neither include nor link them).
target_link_libraries(rx_asset PRIVATE
  $<BUILD_INTERFACE:rx::cgltf> $<BUILD_INTERFACE:rx::stb>)

# Only pack.cc uses miniz; the zip-archive/stdio/time surface is compiled out.
# NO_ZLIB_COMPATIBLE_NAMES matters: without it miniz #defines crc32/compress
# etc. and rewrites unrelated identifiers in any TU that includes it.
target_include_directories(rx_asset PRIVATE ${PROJECT_SOURCE_DIR}/third_party/miniz)
target_compile_definitions(rx_asset PRIVATE
  MINIZ_NO_ARCHIVE_APIS MINIZ_NO_STDIO MINIZ_NO_TIME
  MINIZ_NO_ZLIB_COMPATIBLE_NAMES)

if(NOT MSVC)
  # vendored single header libraries are not warning clean
  set_source_files_properties(gltf_loader.cc PROPERTIES COMPILE_OPTIONS -Wno-shadow)
  set_source_files_properties(${PROJECT_SOURCE_DIR}/third_party/miniz/miniz.c
    PROPERTIES COMPILE_OPTIONS -w)
endif()
