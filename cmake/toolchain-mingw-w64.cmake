# Cross toolchain: x86_64-w64-mingw32 (ucrt) via the nixpkgs cross gcc
# wrapper. Builds rx as Windows PEs (static, or DLLs with -DRX_SHARED=ON) that
# run under Wine. Configure inside `nix develop .#mingw`, which provides the
# compiler, the Windows libraries and the host shader tools:
#   cmake -B build/mingw -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64.cmake \
#     -DRX_NRD=OFF -DRX_DLSS=OFF -DRX_FSR3=OFF -DRX_MIMALLOC=OFF -DRX_FETCH_SDL3=ON \
#     -DRX_RXPACK=$PWD/build/linux/apps/rxpack/rxpack
# RX_RXPACK is a host rxpack that packs the engine archives (Data/*.rxp), which
# the cross-built one cannot run to do. Add -DRX_SHARED=ON for DLLs.
# RX_MINGW_CC, RX_MINGW_MCF, RX_MINGW_MCF_DEV and RX_MINGW_ROOTS come from that
# shell's environment; -D overrides them.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

foreach(_var RX_MINGW_CC RX_MINGW_MCF RX_MINGW_MCF_DEV RX_MINGW_ROOTS)
  if(NOT DEFINED ${_var} AND DEFINED ENV{${_var}})
    set(${_var} "$ENV{${_var}}")
  endif()
endforeach()
if(NOT RX_MINGW_CC OR NOT RX_MINGW_MCF OR NOT RX_MINGW_MCF_DEV)
  message(FATAL_ERROR "toolchain-mingw-w64: RX_MINGW_CC/MCF/MCF_DEV unset; "
                      "configure inside `nix develop .#mingw`")
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
# COFF caps an object at 32k sections; unity sources like harfbuzz.cc exceed it.
add_compile_options(-Wa,-mbig-obj)

set(CMAKE_C_COMPILER "${RX_MINGW_CC}/bin/x86_64-w64-mingw32-gcc")
set(CMAKE_CXX_COMPILER "${RX_MINGW_CC}/bin/x86_64-w64-mingw32-g++")
set(CMAKE_RC_COMPILER "${RX_MINGW_CC}/bin/x86_64-w64-mingw32-windres" CACHE FILEPATH "" )

# CMake's compiler checks re-read this file in a scratch project; forward the
# overrides so they see the same store paths.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES RX_MINGW_CC RX_MINGW_MCF RX_MINGW_MCF_DEV RX_MINGW_ROOTS)
# mcfgthread ships separately from the gcc wrapper in nixpkgs cross. *_INIT
# rather than add_link_options so the compiler checks link too.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-L${RX_MINGW_MCF}/lib")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-L${RX_MINGW_MCF}/lib")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-L${RX_MINGW_MCF}/lib")
# Self-contained test binaries: no libstdc++/libgcc DLL hunting under Wine.
add_link_options(-static-libstdc++ -static-libgcc)

# Target packages come from the mingw sysroot or are fetched, never from the
# host dev shell (whose SDL3, OpenSSL, ... are Linux builds). The *_MODE_* lines
# below only take effect with a root path set.
set(CMAKE_FIND_ROOT_PATH "${RX_MINGW_CC}" "${RX_MINGW_MCF}" ${RX_MINGW_ROOTS})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
