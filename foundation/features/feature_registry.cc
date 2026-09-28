#include "foundation/features/feature_registry.h"

#include <stdlib.h>

#include "foundation/logging/log.h"

namespace rx {
namespace {

// The one and only table. Each entry is direct-initialized in place, so
// base::Feature's deleted copy/move is never needed. Non-const because the env
// override flips `enabled`. This object is referenced by InitFeatures() which
// runs from engine startup, so the linker keeps it: no static-init stripping.
base::Feature g_features[] = {
#define RX_FEATURE(id, name, enabled) {name, enabled},
#include "base/strings/string_ref.h"
#include "foundation/features/features.def"
#undef RX_FEATURE
};

constexpr unsigned kCount = static_cast<unsigned>(FeatureId::kCount);
static_assert(sizeof(g_features) / sizeof(g_features[0]) == kCount,
              "features.def and FeatureId disagree on the flag count");

// Flip the flag whose name matches `name`. Returns false if there is no such
// flag.
bool ApplyOne(base::StringRef name, bool enabled) {
  for (auto& f : g_features) {
    if (name == f.name) {
      f.enabled = enabled;
      return true;
    }
  }
  return false;
}

}  // namespace

bool FeatureEnabled(FeatureId id) {
  return g_features[static_cast<unsigned>(id)].enabled;
}

base::Span<base::Feature> Features() {
  return base::Span<base::Feature>(g_features);
}

void InitFeatures() {
  const char* spec = ::getenv("RX_FEATURES");
  if (!spec || !*spec) return;

  base::StringRef rest(spec);
  while (!rest.empty()) {
    const auto end = rest.find_first_of(", \t");
    base::StringRef tok = rest.substr(0, end);
    rest = end == base::StringRef::npos ? base::StringRef{} : rest.substr(end + 1);
    if (tok.empty()) continue;

    bool enabled = true;
    if (tok.front() == '+') {
      tok.remove_prefix(1);
    } else if (tok.front() == '-') {
      enabled = false;
      tok.remove_prefix(1);
    } else if (const auto eq = tok.find('='); eq != base::StringRef::npos) {
      base::StringRef val = tok.substr(eq + 1);
      enabled = !(val == "0" || val == "false" || val == "off");
      tok = tok.substr(0, eq);
    }
    if (tok.empty()) continue;

    if (ApplyOne(tok, enabled))
      RX_INFO("feature '{}' {} by RX_FEATURES", tok, enabled ? "on" : "off");
    else
      RX_WARN("RX_FEATURES: unknown feature '{}'", tok);
  }
}

}  // namespace rx
