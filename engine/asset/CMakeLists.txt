rx_add_module(asset
  vfs.cc
  pack.cc
  engine_archives.cc
  asset_database.cc
  primitives.cc
  simplify.cc
  subdivide.cc
  gltf_loader.cc
  usd_loader.cc
  blend_import.cc
  materialx.cc
  # miniz compiles straight into the module (hidden visibility keeps its
  # symbols internal); no separate archive to bundle for installed consumers.
  ${PROJECT_SOURCE_DIR}/third_party/miniz/miniz.c
)

target_link_libraries(rx_asset PUBLIC rx::core)
# Last-resort search directory for the engine's own .rxp archives (the CMake
# target that builds them writes them here); RX_ENGINE_ARCHIVES and the working
# directory come first. See engine_archives.cc.
target_compile_definitions(rx_asset PRIVATE
  RX_ENGINE_ARCHIVES_DIR_DEFAULT="${PROJECT_BINARY_DIR}")
# Header-only, PRIVATE: only needed to compile rx_asset itself. Wrapped in
# BUILD_INTERFACE so they never reach the installed export (find_package
# consumers neither include nor link them).
target_link_libraries(rx_asset PRIVATE
  $<BUILD_INTERFACE:rx::cgltf> $<BUILD_INTERFACE:rx::stb_impl>)

# Only pack.cc uses miniz; the zip-archive/stdio/time surface is compiled out.
# NO_ZLIB_COMPATIBLE_NAMES matters: without it miniz #defines crc32/compress
# etc. and rewrites unrelated identifiers in any TU that includes it.
target_include_directories(rx_asset PRIVATE ${PROJECT_SOURCE_DIR}/third_party/miniz)
target_compile_definitions(rx_asset PRIVATE
  MINIZ_NO_ARCHIVE_APIS MINIZ_NO_STDIO MINIZ_NO_TIME
  MINIZ_NO_ZLIB_COMPATIBLE_NAMES)

# usd_loader.cc keeps compiling without tinyusdz (LoadUsdScene then reports the
# build has no USD support), so RX_USD only decides whether the reader is linked.
if(RX_USD)
  target_link_libraries(rx_asset PRIVATE $<BUILD_INTERFACE:tinyusdz_static>)
  # tinyusdz exports no include directory of its own.
  target_include_directories(rx_asset PRIVATE
    ${PROJECT_SOURCE_DIR}/third_party/tinyusdz/src)
  # tinyusdz's headers carry expected-lite, which switches to std::expected
  # whenever the standard has one. The library is compiled as C++17 and picks
  # nonstd::expected; this TU is C++23 and would pick std::expected, so the two
  # would disagree on the type in every shared inline. 1 = nsel_EXPECTED_NONSTD.
  # (optional-lite is already pinned to nonstd upstream, string-view-lite
  # resolves to std::string_view under both standards.)
  set_source_files_properties(usd_loader.cc PROPERTIES
    COMPILE_DEFINITIONS "RX_HAVE_USD;nsel_CONFIG_SELECT_EXPECTED=1")
endif()

if(NOT MSVC)
  # vendored single header libraries are not warning clean
  set_source_files_properties(gltf_loader.cc PROPERTIES COMPILE_OPTIONS -Wno-shadow)
  set_source_files_properties(${PROJECT_SOURCE_DIR}/third_party/miniz/miniz.c
    PROPERTIES COMPILE_OPTIONS -w)
endif()
