#ifndef RX_FOUNDATION_BUILD_CONFIG_EXPORT_H_
#define RX_FOUNDATION_BUILD_CONFIG_EXPORT_H_

// Per-module symbol-export annotations for the RX_SHARED shared-library build
// (the "DLL test"). In the default static build RX_SHARED_BUILD is undefined
// and every macro below expands to nothing, so annotations cost nothing.
//
// Under -DRX_SHARED=ON each rx_<module> is a shared object compiled with hidden
// visibility. A symbol crosses the DSO boundary (is callable from another rx
// module, the viewer, or an embedding game) only when its declaration carries
// that module's RX_<MODULE>_EXPORT macro. Annotate the out-of-line members of
// classes and the free functions another DSO actually references; templates and
// inline-only APIs are instantiated in the consumer and need no annotation.
//
// rx_add_module (cmake/modules.cmake) defines each module's macro as a compile
// definition: RX_<MODULE>_EXPORT=RX_DSO_EXPORT for the module's own sources and
// RX_DSO_IMPORT for everything linking it. There is no per-module line here, so
// adding a module (rx's, a game's, a plugin's) edits no shared file. On ELF the
// two sides are identical (visibility("default")); the split only matters for
// MSVC dllexport/dllimport.

#if defined(RX_SHARED_BUILD)
#if defined(_WIN32)
#define RX_DSO_EXPORT __declspec(dllexport)
#define RX_DSO_IMPORT __declspec(dllimport)
#else
#define RX_DSO_EXPORT __attribute__((visibility("default")))
#define RX_DSO_IMPORT __attribute__((visibility("default")))
#endif
#else
#define RX_DSO_EXPORT
#define RX_DSO_IMPORT
#endif

#endif  // RX_FOUNDATION_BUILD_CONFIG_EXPORT_H_
