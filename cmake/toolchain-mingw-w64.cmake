# Cross toolchain: x86_64-w64-mingw32 via the nixpkgs cross gcc wrapper, used
# to build the d3d12 RHI acceptance tests as Windows PEs and run them under
# Wine (box64-emulated on aarch64 hosts). Configure inside `nix develop` so
# dxc/glslang stay the host tools:
#   cmake -B build/mingw -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64.cmake \
#     -DRX_MODULES=core\;asset\;render -DRX_RHI_VULKAN=OFF -DRX_RHI_D3D12=ON \
#     -DRX_BUILD_TESTS=ON -DRX_BUILD_RUNTIME=OFF
# The MINGW_CC/MINGW_MCF paths can be overridden with -DRX_MINGW_CC=... /
# -DRX_MINGW_MCF=... when the store paths differ.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT DEFINED RX_MINGW_CC)
  set(RX_MINGW_CC "/nix/store/ha64priyfaxkdg7ylvwz3pc0p5vw9nfz-x86_64-w64-mingw32-gcc-wrapper-15.2.0")
endif()
if(NOT DEFINED RX_MINGW_MCF)
  set(RX_MINGW_MCF "/nix/store/y8iv45r55ryaxgwdxjkbbprmcd8jyl91-mcfgthread-x86_64-w64-mingw32-2.4.1")
endif()
if(NOT DEFINED RX_MINGW_MCF_DEV)
  set(RX_MINGW_MCF_DEV "/nix/store/75dflqgi4q9saldsdzpwx56wf6cnpfhv-mcfgthread-x86_64-w64-mingw32-2.4.1-dev")
endif()
# libstdc++'s gthr model on this toolchain is mcfgthread; its headers live in
# the dev output. SHELL: keeps each "-isystem <dir>" pair intact through
# CMake's COMPILE_OPTIONS de-duplication (a repeated bare "-isystem" token
# would be dropped, orphaning the second path).
add_compile_options("SHELL:-isystem ${RX_MINGW_MCF_DEV}/include")
# Case shims (<Windows.h> -> <windows.h>) for MSVC-cased includes.
add_compile_options("SHELL:-isystem ${CMAKE_CURRENT_LIST_DIR}/mingw-shims")
# gcc has no MSVC __int64 builtin (equilibrium's minwin.h relies on it).
add_compile_options("-D__int64=long long")

set(CMAKE_C_COMPILER "${RX_MINGW_CC}/bin/x86_64-w64-mingw32-gcc")
set(CMAKE_CXX_COMPILER "${RX_MINGW_CC}/bin/x86_64-w64-mingw32-g++")
set(CMAKE_RC_COMPILER "${RX_MINGW_CC}/bin/x86_64-w64-mingw32-windres" CACHE FILEPATH "" )

# mcfgthread ships separately from the gcc wrapper in nixpkgs cross.
add_link_options("-L${RX_MINGW_MCF}/lib")
# Self-contained test binaries: no libstdc++/libgcc DLL hunting under Wine.
add_link_options(-static-libstdc++ -static-libgcc)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
