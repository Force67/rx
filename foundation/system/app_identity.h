#ifndef RX_FOUNDATION_SYSTEM_APP_IDENTITY_H_
#define RX_FOUNDATION_SYSTEM_APP_IDENTITY_H_

#include "base/strings/string_ref.h"
#include "base/strings/xstring.h"
#include "foundation/build_config/export.h"

namespace rx {

// Who runs on the engine, three ways:
//   id     a cuid2 (base::GenerateCuid2, `rx --new-app-id`): unique across every
//          rx app and stable across renames, for what only machines see (the
//          cache folder).
//   name   a slug, for what users see: the vfs namespace <name>:// and the
//          settings folder (~/.config/<name>).
//   title  what a person reads: window title, the application name drivers and
//          compositors see.
// host::Host sets it from HostConfig before anything asks; rx's own viewer and
// editor carry one too. Until then it is rx's, with the fused test id.
struct AppIdentity {
  base::String id;
  base::String name;
  base::String title;
};

// Lowercase letters, digits, '-' and '_', starting with a letter: a mount name
// and a directory name everywhere. "rxe" and "user" are the engine's mounts.
RX_FOUNDATION_EXPORT bool IsValidAppName(base::StringRef name);

RX_FOUNDATION_EXPORT void SetAppIdentity(AppIdentity identity);
RX_FOUNDATION_EXPORT const AppIdentity& GetAppIdentity();

// The app's writable per-user folders, created on first use:
//   config  $XDG_CONFIG_HOME/<name>, %APPDATA%\<name>, ~/Library/Application Support/<name>
//   cache   $XDG_CACHE_HOME/<id>, %LOCALAPPDATA%\<id>, ~/Library/Caches/<id>
// Settings a player changes live in config, under the name they recognize
// (controls.ini, config/*.ini); what can be rebuilt lives in cache, under the
// id no other app shares (the pipeline cache).
RX_FOUNDATION_EXPORT base::String UserConfigDirectory();
RX_FOUNDATION_EXPORT base::String UserCacheDirectory();

}  // namespace rx

#endif  // RX_FOUNDATION_SYSTEM_APP_IDENTITY_H_
