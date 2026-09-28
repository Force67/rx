#include "foundation/system/app_identity.h"

#include <stdlib.h>

#include <base/hashing/cuid2.h>

#include "base/memory/move.h"
#include "foundation/files/file_system.h"

namespace rx {
namespace {

AppIdentity& Identity() {
  static AppIdentity identity{base::kFusedTestCuid2, "rx", "rx"};
  return identity;
}

// The platform's base folder for `kind`, "" when the environment names none.
base::String UserBase(bool config) {
#if defined(_WIN32)
  const char* dir = ::getenv(config ? "APPDATA" : "LOCALAPPDATA");
  return dir ? base::String(dir) : base::String();
#elif defined(__APPLE__)
  const char* home = ::getenv("HOME");
  if (!home) return {};
  return fs::Join(home, config ? "Library/Application Support" : "Library/Caches");
#else
  if (const char* xdg = ::getenv(config ? "XDG_CONFIG_HOME" : "XDG_CACHE_HOME"); xdg && *xdg)
    return xdg;
  const char* home = ::getenv("HOME");
  if (!home) return {};
  return fs::Join(home, config ? ".config" : ".cache");
#endif
}

base::String UserDirectory(bool config) {
  const base::String base = UserBase(config);
  const AppIdentity& app = GetAppIdentity();
  const base::String dir =
      fs::Join(base.empty() ? fs::TempDirectory() : base, config ? app.name : app.id);
  fs::CreateDirectories(dir);
  return dir;
}

}  // namespace

bool IsValidAppName(base::StringRef name) {
  if (name.empty() || name == "rxe" || name == "user" || name[0] < 'a' || name[0] > 'z')
    return false;
  for (char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}

void SetAppIdentity(AppIdentity identity) { Identity() = base::move(identity); }

const AppIdentity& GetAppIdentity() { return Identity(); }

base::String UserConfigDirectory() { return UserDirectory(true); }

base::String UserCacheDirectory() { return UserDirectory(false); }

}  // namespace rx
