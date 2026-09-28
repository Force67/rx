rx_add_module(asset
  vfs.cc
  pack.cc
  content_mounts.cc
  asset_database.cc
  primitives.cc
  procedural_texture.cc
  image_file.cc
  exr_write.cc
  bc_encode.cc
  texture_compress.cc
  simplify.cc
  subdivide.cc
  # miniz compiles straight into the module (hidden visibility keeps its
  # symbols internal); no separate archive to bundle for installed consumers.
  ${PROJECT_SOURCE_DIR}/third_party/miniz/miniz.c
)

target_link_libraries(rx_asset PUBLIC rx::foundation)
# Header-only, PRIVATE: only needed to compile rx_asset itself. Wrapped in
# BUILD_INTERFACE so they never reach the installed export (find_package
# consumers neither include nor link them).
target_link_libraries(rx_asset PRIVATE $<BUILD_INTERFACE:rx::stb_impl>)

# Only pack.cc uses miniz; the zip-archive/stdio/time surface is compiled out.
# NO_ZLIB_COMPATIBLE_NAMES matters: without it miniz #defines crc32/compress
# etc. and rewrites unrelated identifiers in any TU that includes it.
target_include_directories(rx_asset PRIVATE ${PROJECT_SOURCE_DIR}/third_party/miniz)
target_compile_definitions(rx_asset PRIVATE
  MINIZ_NO_ARCHIVE_APIS MINIZ_NO_STDIO MINIZ_NO_TIME
  MINIZ_NO_ZLIB_COMPATIBLE_NAMES)

if(NOT MSVC)
  # vendored single header libraries are not warning clean
  set_source_files_properties(${PROJECT_SOURCE_DIR}/third_party/miniz/miniz.c
    PROPERTIES COMPILE_OPTIONS -w)
endif()

if(RX_BUILD_TESTS)
  # Vfs mount points + .rxp pack round trip, pure CPU: no driver or window
  # needed, runs everywhere plain `ctest` runs.
  add_executable(vfs_test vfs_test.cc)
  target_link_libraries(vfs_test PRIVATE rx::asset rx::foundation)
  add_test(NAME vfs_test COMMAND vfs_test)

  add_executable(asset_database_test asset_database_test.cc)
  target_link_libraries(asset_database_test PRIVATE rx::asset rx::foundation)
  add_test(NAME asset_database_test COMMAND asset_database_test)

  # Procedural texture synthesis and the blockout primitives behind the
  # .rxscene Pattern/Shape components.
  add_executable(procedural_asset_test procedural_asset_test.cc)
  target_link_libraries(procedural_asset_test PRIVATE rx::asset rx::foundation)
  add_test(NAME procedural_asset_test COMMAND procedural_asset_test)

  # BCn block encoders and the import-time compressor, against decoders written
  # from the format definitions. No gpu involved, so this one really runs under
  # a plain ctest.
  add_executable(bc_encode_test bc_encode_test.cc)
  target_link_libraries(bc_encode_test PRIVATE rx::asset rx::foundation)
  add_test(NAME bc_encode_test COMMAND bc_encode_test)
endif()
