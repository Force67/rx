#ifndef RX_FOUNDATION_FEATURES_FEATURE_REGISTRY_H_
#define RX_FOUNDATION_FEATURES_FEATURE_REGISTRY_H_

#include "foundation/build_config/export.h"
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

// Feature flags. A module declares each flag at namespace scope in the file that
// reads it, and reads it as a bool:
//
//   base::Feature ClothFeature{"physics.cloth", /*enabled=*/true};
//   ...
//   if (!ClothFeature) return {};
//
// base::Feature registers itself on one process-wide chain (equilibrium makes
// the chain root unique across shared objects), so there is no central list to
// edit. A flag lives exactly as long as the code that reads it: if the linker
// drops an unreferenced object, it drops the flag with the only reader.

namespace rx {

// Apply RX_FEATURES overrides once, early in startup and before any flag is
// read. Tokens are comma or whitespace separated: "name" / "+name" enable,
// "-name" / "name=0" disable. A name no flag in this binary carries is warned
// about and skipped.
RX_FOUNDATION_EXPORT void InitFeatures();

}  // namespace rx

#endif  // RX_FOUNDATION_FEATURES_FEATURE_REGISTRY_H_
