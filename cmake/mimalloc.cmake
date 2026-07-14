# mimalloc integration (RX_MIMALLOC, on by default). The engine routes
# allocations through mimalloc for faster, lower-fragmentation heap use and
# per-category memory tracking (engine/core/memory/).
#
# Two layers of coverage (see the design note in the perf discussion):
#   Layer 1 - C++ operator new/delete: engine/core/memory/new_override.cc,
#             compiled into each executable on every platform. Routes to
#             mimalloc and charges the memory tracker's thread-local category
#             using mi_usable_size on both sides.
#   Layer 2 - the raw C malloc/free used by third-party libraries and the hosted
#             CLR's native allocations. POSIX: link mimalloc-static WHOLE so its
#             strong malloc/free symbols interpose libc's weak ones across the
#             whole process, including later dlopen'd libraries (the CLR). macOS:
#             the same static library registers a malloc zone at load. Windows has
#             no symbol interposition, so we ship mimalloc.dll + the prebuilt
#             mimalloc-redirect.dll beside the executable (the redirect patches
#             the CRT allocator at startup).
#
# The helper rx_enable_mimalloc(<target>) is always defined and is a
# no-op when the option is off, so call sites stay unconditional.

if(RX_MIMALLOC)
  # Prefer a system/toolchain mimalloc (e.g. from the nix dev shell); otherwise
  # build a pinned copy from source so every CI platform gets the same allocator.
  find_package(mimalloc 2.1 CONFIG QUIET)
  if(NOT mimalloc_FOUND)
    set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
    set(MI_OVERRIDE ON CACHE BOOL "" FORCE)  # define malloc/free + new/delete
    # Keep both the static lib (POSIX interpose) and the shared DLL (Windows).
    FetchContent_Declare(mimalloc
      GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
      GIT_TAG v2.1.7
      GIT_SHALLOW ON)
    FetchContent_MakeAvailable(mimalloc)
  endif()
  # On ELF the static override archive also carries the mangled C++ operator
  # symbols (_Znwm/_ZdlPv...), which would collide with the tracked overrides
  # in new_override.cc. Link a copy with those demoted to local symbols:
  # mimalloc keeps malloc/free interposition, rx provides the (tracking)
  # operator layer on top. They only forwarded to the same mi_new/mi_free
  # anyway, so behavior is unchanged. macOS interposes via malloc zones and
  # never defines them; Windows uses the redirect DLL.
  if(TARGET mimalloc-static AND UNIX AND NOT APPLE)
    set(RX_MIMALLOC_PATCHED ${CMAKE_BINARY_DIR}/libmimalloc-rx.a)
    add_custom_command(OUTPUT ${RX_MIMALLOC_PATCHED}
      COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:mimalloc-static> ${RX_MIMALLOC_PATCHED}
      COMMAND ${CMAKE_OBJCOPY} --wildcard
              --localize-symbol=_Zn* --localize-symbol=_Zd*
              ${RX_MIMALLOC_PATCHED}
      DEPENDS mimalloc-static $<TARGET_FILE:mimalloc-static>
      COMMENT "mimalloc: localizing C++ operator symbols (rx overrides them)"
      VERBATIM)
    add_custom_target(rx_mimalloc_patched DEPENDS ${RX_MIMALLOC_PATCHED})
  endif()
  message(STATUS "mimalloc enabled")
endif()

# Applies the enabled mimalloc coverage to one executable target.
function(rx_enable_mimalloc target)
  if(NOT RX_MIMALLOC)
    return()
  endif()
  target_compile_definitions(${target} PRIVATE RX_MIMALLOC=1)
  # Layer 1: the tracked operator new/delete override, compiled into this
  # binary on every platform (global operators must be defined once per
  # binary; the exe's definitions preempt libstdc++'s process-wide on ELF).
  target_sources(${target} PRIVATE ${PROJECT_SOURCE_DIR}/engine/core/memory/new_override.cc)
  target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/engine)
  if(WIN32)
    # Layer 2: dynamic override via the DLL, whose redirect patches the CRT.
    target_link_libraries(${target} PRIVATE mimalloc)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_FILE:mimalloc> $<TARGET_FILE_DIR:${target}>
      COMMENT "mimalloc: staging mimalloc.dll beside ${target}"
      VERBATIM)
    # mimalloc-redirect.dll is a prebuilt binary shipped in the mimalloc tree; it
    # must sit next to mimalloc.dll for the CRT patch to take effect.
    if(DEFINED mimalloc_SOURCE_DIR AND
       EXISTS "${mimalloc_SOURCE_DIR}/bin/mimalloc-redirect.dll")
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${mimalloc_SOURCE_DIR}/bin/mimalloc-redirect.dll" $<TARGET_FILE_DIR:${target}>
        VERBATIM)
    endif()
  else()
    # Layer 2: link the static override whole, so mimalloc's malloc/free
    # replace libc's across the process (covers third-party C allocations and
    # the hosted CLR; operator new/delete come from layer 1 above).
    if(TARGET rx_mimalloc_patched)
      add_dependencies(${target} rx_mimalloc_patched)
      target_link_libraries(${target} PRIVATE
        $<LINK_LIBRARY:WHOLE_ARCHIVE,${CMAKE_BINARY_DIR}/libmimalloc-rx.a>)
      # Headers still come from the real target; only the archive is swapped.
      target_include_directories(${target} PRIVATE
        $<TARGET_PROPERTY:mimalloc-static,INTERFACE_INCLUDE_DIRECTORIES>)
    else()
      target_link_libraries(${target} PRIVATE $<LINK_LIBRARY:WHOLE_ARCHIVE,mimalloc-static>)
    endif()
  endif()
endfunction()
