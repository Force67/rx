rx_add_module(usd usd_loader.cc)
target_link_libraries(rx_usd PUBLIC rx::asset rx::foundation)
target_link_libraries(rx_usd PRIVATE $<BUILD_INTERFACE:rx::stb_impl>)

# usd_loader.cc keeps compiling without tinyusdz (LoadUsdScene then reports the
# build has no USD support), so RX_USD only decides whether the reader is linked.
if(RX_USD)
  target_link_libraries(rx_usd PRIVATE $<BUILD_INTERFACE:tinyusdz_static>)
  # tinyusdz exports no include directory of its own.
  target_include_directories(rx_usd PRIVATE
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
