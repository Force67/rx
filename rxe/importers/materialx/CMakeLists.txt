rx_add_module(materialx materialx.cc)
target_link_libraries(rx_materialx PUBLIC rx::asset rx::foundation)

if(RX_BUILD_TESTS)
  add_executable(materialx_test materialx_test.cc)
  target_link_libraries(materialx_test PRIVATE rx::materialx rx::asset rx::foundation)
  add_test(NAME materialx_test COMMAND materialx_test)

  # OpenPBR Surface import from MaterialX: the open_pbr_surface mapping, its unit
  # and parametrization conversions, the spec defaults for unauthored inputs, and
  # that standard_surface documents keep the legacy path. No USD needed.
  add_executable(openpbr_test openpbr_test.cc)
  target_link_libraries(openpbr_test PRIVATE rx::materialx rx::asset rx::foundation)
  add_test(NAME openpbr_test COMMAND openpbr_test)
endif()
