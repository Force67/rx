#ifndef RX_CORE_FEATURE_REGISTRY_H_
#define RX_CORE_FEATURE_REGISTRY_H_

#include <base/containers/span.h>

#include "core/export.h"
// base/feature.h's constructor parameters shadow its members, so the header is
// not -Wshadow clean. Silence just this include rather than spraying the
// warning across every consumer or patching the vendored library.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4458)
#endif
#include <base/feature.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

// A linker-proof feature-flag registry.
//
// base::Feature self-registers through InitChain, but that registration is a
// static initializer: when a flag lives in a static library and nothing else
// references its object file, the linker drops the object (and the
// registration with it). This registry sidesteps that entirely. Every flag is
// declared in core/features.def and lives in one table in one translation
// unit; InitFeatures() is called from engine startup, so that object is always
// pulled into the link and the table is always complete. We never walk the
// InitChain.

namespace rx {

// Stable handle for every flag in core/features.def.
enum class FeatureId : unsigned {
#define RX_FEATURE(id, name, enabled) k##id,
#include "core/features.def"
#undef RX_FEATURE
  kCount
};

// True when the flag is on (default from the manifest, after env overrides).
// A single array read, no list walk.
//
// The g_features table backing these is an anonymous-namespace global in
// feature_registry.cc, reached only through these three functions, so under
// RX_SHARED it stays a single instance inside the core DSO (verified: no other
// translation unit can name the array).
RX_CORE_EXPORT bool FeatureEnabled(FeatureId id);

// The whole table, for listing in the debug UI. size() == kCount.
RX_CORE_EXPORT base::Span<base::Feature> Features();

// Apply RX_FEATURES overrides once, early in engine startup. Tokens are comma
// or whitespace separated: "name" / "+name" enable, "-name" / "name=0" disable.
// Unknown names are warned about and skipped.
RX_CORE_EXPORT void InitFeatures();

}  // namespace rx

#endif  // RX_CORE_FEATURE_REGISTRY_H_
