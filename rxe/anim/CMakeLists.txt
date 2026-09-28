rx_add_module(anim
  pose.cc
  procedural_gait.cc
  foot_ik.cc
  anim_graph.cc
  rig_player.cc
  foot_placement.cc
  morph.cc
  expression.cc
  body_dynamics.cc
)
target_link_libraries(rx_anim PUBLIC rx::foundation rx::asset)
# kinema is a PRIVATE implementation detail of the graph/rig/foot-placement units
# (it never appears in a public anim header, mirroring how physics consumes it).
# BUILD_INTERFACE keeps it out of the installed export; rxConfig re-attaches the
# bundled imported archive for find_package consumers.
target_link_libraries(rx_anim PRIVATE $<BUILD_INTERFACE:kinema::kinema>)

if(RX_BUILD_TESTS AND (RX_RHI_VULKAN OR RX_RHI_D3D12))
  # Morph targets: glTF import + weight-track sampling against the committed
  # CC0 Khronos AnimatedMorphCube, and the shared morph.hlsli accumulation
  # replayed in a compute shader against a CPU reference. The GPU section is
  # Vulkan-only (buffer-device-address reads, like the skinned path) and skips
  # without a driver.
  add_executable(morph_test morph_test.cc)
  target_link_libraries(morph_test PRIVATE rx::gltf rx::render rx::anim rx::asset rx::foundation)
  set(RX_SHADER_INCLUDE_DIRS
    ${PROJECT_SOURCE_DIR}/rxe/render/shaders
    ${PROJECT_SOURCE_DIR}/rxe/render/shaders/pipeline)
  set(RX_SHADER_NO_DXIL morph_apply.cs.hlsl)
  set(RX_SHADER_DEPS_morph_apply_cs_hlsl ../render/shaders/pipeline/morph.hlsli)
  rx_embed_shaders(morph_test morph_apply.cs.hlsl)
  unset(RX_SHADER_INCLUDE_DIRS)
  unset(RX_SHADER_NO_DXIL)
  add_test(NAME morph_test
           COMMAND morph_test ${PROJECT_SOURCE_DIR}/testing/data/AnimatedMorphCube.glb)
endif()

if(RX_BUILD_TESTS)
  # Expression controller: pose transitions converge and stay C1 through a
  # mid-flight retarget, outputs stay clamped, seeded blinks land inside their
  # window (and are absorbed by a closed-eyes pose) and a fixed seed replays
  # bit-identically. Pure CPU.
  add_executable(expression_test expression_test.cc)
  target_link_libraries(expression_test PRIVATE rx::anim rx::asset rx::foundation)
  add_test(NAME expression_test COMMAND expression_test)

  add_executable(walk_style_test walk_style_test.cc)
  target_link_libraries(walk_style_test PRIVATE rx::anim rx::asset rx::foundation)
  add_test(NAME walk_style_test COMMAND walk_style_test)

  # Extensible soft-tissue body dynamics: physical equilibrium/inertial lag,
  # analytical frame-rate independence, anatomical clamps + teleport reset,
  # bone layering, source-unit conversion, region presets, and additive
  # deformation morph output. Pure CPU and independent of Jolt/the render backend.
  add_executable(body_dynamics_test body_dynamics_test.cc)
  target_link_libraries(body_dynamics_test PRIVATE rx::anim rx::asset rx::foundation)
  add_test(NAME body_dynamics_test COMMAND body_dynamics_test)

  # Optional end-to-end authoring import test. The normal CTest invocation
  # checks error handling without requiring Blender; pass a .blend path to the
  # executable to convert it and verify its real skin + jiggle response.
  add_executable(blend_jiggle_test blend_jiggle_test.cc)
  target_link_libraries(blend_jiggle_test PRIVATE rx::blend rx::gltf rx::anim rx::asset rx::foundation)
  target_compile_definitions(blend_jiggle_test PRIVATE
    RX_BLEND_CONVERTER_SCRIPT="${PROJECT_SOURCE_DIR}/tools/blend_to_glb.py")
  add_test(NAME blend_jiggle_api_test COMMAND blend_jiggle_test)
endif()
