#include "foundation/features/feature_registry.h"

#include <stdlib.h>

#include "base/strings/string_ref.h"
#include "foundation/logging/log.h"

namespace rx {
namespace {

// Flip the flag whose name matches `name`. Returns false if no flag in this
// binary carries it.
bool ApplyOne(base::StringRef name, bool enabled) {
  bool found = false;
  base::Feature::VisitAll([&](const base::Feature* feature) {
    if (name == feature->name) {
      // The chain hands out const pointers; flags are mutable globals, which is
      // the one thing this function exists to write.
      const_cast<base::Feature*>(feature)->enabled = enabled;
      found = true;
    }
  });
  return found;
}

}  // namespace

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
