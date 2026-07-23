# Shaders compiled to spirv at build time and embedded as C arrays. Two source
# languages share the flow: hlsl through dxc, slang through slangc. The stage
# comes from the file name: <name>.{vs,ps,cs,ms,as}.{hlsl,slang}. Symbols
# follow MAKE_C_IDENTIFIER, e.g. mesh.vs.hlsl embeds as k_mesh_vs_hlsl and
# blit.ps.slang as k_blit_ps_slang in generated/shaders/<symbol>.h. The entry
# point is always `main`.
#
# When the d3d12 backend is enabled (RX_RHI_D3D12) every shader also
# gets a DXIL sidecar embedded as k_<symbol>_dxil. The DXIL target is SM 6.5,
# not 6.6: it is the highest model accepted by vkd3d 2.0 (the Linux D3D12
# layer used for validation) and still covers ray queries (6.5) and mesh
# shaders (6.5). The DXIL is unsigned, which vkd3d accepts natively and
# Windows accepts with experimental shader models enabled; production Windows
# builds would sign via dxil.dll.
#
# For hlsl the sidecar is the same dxc invocation minus -spirv. For slang it
# is slangc's hlsl target fed through the same dxc: distro slangc builds
# (nixpkgs among them) ship without the embedded-dxc DXIL backend, and slang's
# own DXIL path is that same lower-to-hlsl-then-dxc flow anyway, just done
# where we can see it. -matrix-layout-column-major pins slang to dxc's default
# so both languages agree with the CPU-side constant layout.

# Slang spells stages out; the short file-suffix forms are dxc profiles.
set(RX_SLANG_STAGE_vs vertex)
set(RX_SLANG_STAGE_ps fragment)
set(RX_SLANG_STAGE_cs compute)
set(RX_SLANG_STAGE_ms mesh)
set(RX_SLANG_STAGE_as amplification)

function(rx_embed_shaders target)
  # Optional -I include dirs for shaders that pull in vendored headers (e.g.
  # NRD.hlsli), passed via the RX_SHADER_INCLUDE_DIRS list variable. dxc and
  # slangc share the flag spelling.
  set(include_flags)
  foreach(dir ${RX_SHADER_INCLUDE_DIRS})
    list(APPEND include_flags -I ${dir})
  endforeach()
  set(headers)
  foreach(shader ${ARGN})
    get_filename_component(name ${shader} NAME)
    string(MAKE_C_IDENTIFIER ${name} symbol)
    if(name MATCHES "\\.(vs|ps|cs|ms|as)\\.(hlsl|slang)$")
      set(stage ${CMAKE_MATCH_1})
      set(lang ${CMAKE_MATCH_2})
    else()
      message(FATAL_ERROR "cannot derive a shader stage from ${name}")
    endif()
    if(lang STREQUAL "slang" AND NOT RX_SLANGC)
      message(FATAL_ERROR
        "slangc is required to compile ${name}; install shader-slang or set RX_SLANGC")
    endif()
    set(src ${CMAKE_CURRENT_SOURCE_DIR}/${shader})
    set(spv ${CMAKE_CURRENT_BINARY_DIR}/shaders/${name}.spv)
    set(header ${CMAKE_BINARY_DIR}/generated/shaders/${symbol}.h)
    # hlsl #include dependencies are not tracked automatically; wrapper
    # shaders (variant defines around a shared body) list their includes via
    # RX_SHADER_DEPS_<symbol> so edits to the body rebuild the variant. slang
    # tracks includes and imports itself through -depfile.
    set(extra_deps)
    if(DEFINED RX_SHADER_DEPS_${symbol})
      foreach(dep ${RX_SHADER_DEPS_${symbol}})
        list(APPEND extra_deps ${CMAKE_CURRENT_SOURCE_DIR}/${dep})
      endforeach()
    endif()
    if(lang STREQUAL "slang")
      add_custom_command(OUTPUT ${spv}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/shaders
        COMMAND ${RX_SLANGC} ${src} -target spirv -profile sm_6_6+spirv_1_6
                -entry main -stage ${RX_SLANG_STAGE_${stage}}
                -matrix-layout-column-major ${include_flags}
                -depfile ${spv}.d -o ${spv}
        DEPFILE ${spv}.d
        DEPENDS ${src} ${extra_deps}
        COMMENT "slang ${name}")
    else()
      add_custom_command(OUTPUT ${spv}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/shaders
        COMMAND ${RX_DXC} -spirv -fspv-target-env=vulkan1.3 -T ${stage}_6_6 -E main
                ${include_flags} -Fo ${spv} ${src}
        DEPENDS ${src} ${extra_deps}
        COMMENT "hlsl ${name}")
    endif()
    set(embed_args)
    set(embed_deps ${spv})
    # Shaders on the RX_SHADER_NO_DXIL list cannot target DXIL (they
    # use SPIR-V-only constructs, chiefly buffer-device-address reads:
    # vk::RawBufferLoad in hlsl, pointers in slang). Their sidecar embeds as a
    # null pointer, which the d3d12 device reports as "pipeline unavailable"
    # instead of failing the build.
    if(RX_RHI_D3D12 AND name IN_LIST RX_SHADER_NO_DXIL)
      list(APPEND embed_args -DDXIL_MISSING=1)
    elseif(RX_RHI_D3D12)
      set(dxil ${CMAKE_CURRENT_BINARY_DIR}/shaders/${name}.dxil)
      if(lang STREQUAL "slang")
        set(gen_hlsl ${CMAKE_CURRENT_BINARY_DIR}/shaders/${name}.dxil.hlsl)
        add_custom_command(OUTPUT ${dxil}
          COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/shaders
          COMMAND ${RX_SLANGC} ${src} -target hlsl
                  -entry main -stage ${RX_SLANG_STAGE_${stage}}
                  -matrix-layout-column-major ${include_flags}
                  -depfile ${dxil}.d -o ${gen_hlsl}
          COMMAND ${RX_DXC} -T ${stage}_6_5 -E main -Qstrip_reflect
                  -Fo ${dxil} ${gen_hlsl}
          DEPFILE ${dxil}.d
          DEPENDS ${src} ${extra_deps}
          COMMENT "dxil ${name}")
      else()
        add_custom_command(OUTPUT ${dxil}
          COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/shaders
          COMMAND ${RX_DXC} -T ${stage}_6_5 -E main -Qstrip_reflect
                  ${include_flags} -Fo ${dxil} ${src}
          DEPENDS ${src} ${extra_deps}
          COMMENT "dxil ${name}")
      endif()
      list(APPEND embed_args -DDXIL=${dxil})
      list(APPEND embed_deps ${dxil})
    endif()
    add_custom_command(OUTPUT ${header}
      COMMAND ${CMAKE_COMMAND} -DSPV=${spv} -DHEADER=${header} -DSYMBOL=${symbol}
              ${embed_args} -P ${PROJECT_SOURCE_DIR}/cmake/embed_spv.cmake
      DEPENDS ${embed_deps} ${PROJECT_SOURCE_DIR}/cmake/embed_spv.cmake
      COMMENT "embed ${name}")
    list(APPEND headers ${header})
  endforeach()
  add_custom_target(${target}_shaders DEPENDS ${headers})
  add_dependencies(${target} ${target}_shaders)
  target_include_directories(${target} PRIVATE ${CMAKE_BINARY_DIR}/generated)
endfunction()
