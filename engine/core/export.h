#ifndef RX_CORE_EXPORT_H_
#define RX_CORE_EXPORT_H_

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
// The macro resolves to the export side inside the owning module (its .cc files
// are compiled with RX_<MODULE>_IMPLEMENTATION defined by rx_add_module) and to
// the import side everywhere else. On ELF the two sides are identical
// (visibility("default")); the split only matters for MSVC dllexport/dllimport.

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

#if defined(RX_CORE_IMPLEMENTATION)
#define RX_CORE_EXPORT RX_DSO_EXPORT
#else
#define RX_CORE_EXPORT RX_DSO_IMPORT
#endif

#if defined(RX_ECS_IMPLEMENTATION)
#define RX_ECS_EXPORT RX_DSO_EXPORT
#else
#define RX_ECS_EXPORT RX_DSO_IMPORT
#endif

#if defined(RX_ASSET_IMPLEMENTATION)
#define RX_ASSET_EXPORT RX_DSO_EXPORT
#else
#define RX_ASSET_EXPORT RX_DSO_IMPORT
#endif

#if defined(RX_SCENE_IMPLEMENTATION)
#define RX_SCENE_EXPORT RX_DSO_EXPORT
#else
#define RX_SCENE_EXPORT RX_DSO_IMPORT
#endif

#if defined(RX_TERRAIN_IMPLEMENTATION)
#define RX_TERRAIN_EXPORT RX_DSO_EXPORT
#else
#define RX_TERRAIN_EXPORT RX_DSO_IMPORT
#endif

#if defined(RX_RENDER_IMPLEMENTATION)
#define RX_RENDER_EXPORT RX_DSO_EXPORT
#else
#define RX_RENDER_EXPORT RX_DSO_IMPORT
#endif

#if defined(RX_PHYSICS_IMPLEMENTATION)
#define RX_PHYSICS_EXPORT RX_DSO_EXPORT
#else
#define RX_PHYSICS_EXPORT RX_DSO_IMPORT
#endif

#if defined(RX_LOCOMOTION_IMPLEMENTATION)
#define RX_LOCOMOTION_EXPORT RX_DSO_EXPORT
#else
#define RX_LOCOMOTION_EXPORT RX_DSO_IMPORT
#endif

#if defined(RX_ANIM_IMPLEMENTATION)
#define RX_ANIM_EXPORT RX_DSO_EXPORT
#else
#define RX_ANIM_EXPORT RX_DSO_IMPORT
#endif

#if defined(RX_AUDIO_IMPLEMENTATION)
#define RX_AUDIO_EXPORT RX_DSO_EXPORT
#else
#define RX_AUDIO_EXPORT RX_DSO_IMPORT
#endif

#if defined(RX_RPC_IMPLEMENTATION)
#define RX_RPC_EXPORT RX_DSO_EXPORT
#else
#define RX_RPC_EXPORT RX_DSO_IMPORT
#endif

#if defined(RX_NET_IMPLEMENTATION)
#define RX_NET_EXPORT RX_DSO_EXPORT
#else
#define RX_NET_EXPORT RX_DSO_IMPORT
#endif

#if defined(RX_NET_VIZ_IMPLEMENTATION)
#define RX_NET_VIZ_EXPORT RX_DSO_EXPORT
#else
#define RX_NET_VIZ_EXPORT RX_DSO_IMPORT
#endif

#if defined(RX_NAV_IMPLEMENTATION)
#  define RX_NAV_EXPORT RX_DSO_EXPORT
#else
#  define RX_NAV_EXPORT RX_DSO_IMPORT
#endif

#if defined(RX_CHARACTER_IMPLEMENTATION)
#  define RX_CHARACTER_EXPORT RX_DSO_EXPORT
#else
#  define RX_CHARACTER_EXPORT RX_DSO_IMPORT
#endif

#if defined(RX_NAV_VIZ_IMPLEMENTATION)
#  define RX_NAV_VIZ_EXPORT RX_DSO_EXPORT
#else
#  define RX_NAV_VIZ_EXPORT RX_DSO_IMPORT
#endif

#if defined(RX_APP_IMPLEMENTATION)
#define RX_APP_EXPORT RX_DSO_EXPORT
#else
#define RX_APP_EXPORT RX_DSO_IMPORT
#endif

#if defined(RX_EDIT_IMPLEMENTATION)
#define RX_EDIT_EXPORT RX_DSO_EXPORT
#else
#define RX_EDIT_EXPORT RX_DSO_IMPORT
#endif

#endif // RX_CORE_EXPORT_H_
