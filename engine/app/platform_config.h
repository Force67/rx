#ifndef RX_ENGINE_APP_PLATFORM_CONFIG_H_
#define RX_ENGINE_APP_PLATFORM_CONFIG_H_

#include <base/containers/vector.h>

#include "asset/vfs.h"
#include "base/strings/string_ref.h"
#include "base/strings/xstring.h"
#include "core/export.h"
#include "render/core/presets.h"

namespace rx::app {

// A platform config: the ini files that decide how the engine and a game run on
// one kind of machine (docs/CONFIG.md). Each section belongs to the subsystem
// that owns its keys:
//   [render] [render.<group>]   RenderSettings, the render::ApplyIni keys
//   [memory.<group>]            the memory plan (mem::ParseMemoryConfigText)
//   [options]                   any registered base::Option, by its name
// `include = <vfs path>` applies another file first, so the including file's
// keys win. Several files read into one config layer the same way: later wins.
struct PlatformConfig {
  base::String render;  // [render*] lines, in override order
  base::String memory;  // [memory.*] lines as [<group>], in override order
  struct Option {
    base::String name;
    base::String value;
    base::String where;  // "path:line", for the error when it does not apply
  };
  base::Vector<Option> options;
  // Lines that went nowhere (unknown section or key, bad value, missing
  // include), each already logged with its path:line.
  int problems = 0;
};

// Reads `path` through `vfs` into `out`, following include lines (at most 8
// deep). A path without a mount (no "://") is read from disk instead, for
// RX_CONFIG; its includes still go through the vfs. False when `path` itself
// does not exist; a missing include only counts as a problem.
RX_APP_EXPORT bool ReadPlatformConfig(const asset::Vfs& vfs, base::StringRef path,
                                      PlatformConfig* out);

// Reads a platform's config files into `out`, in override order:
//   rxe://config/default.ini     the engine, every tier (optional)
//   rxe://config/<tier>.ini      the engine's tier (required)
//   <title>://config/default.ini the game, every tier (optional)
//   <title>://config/<tier>.ini  the game's tier (optional)
//   RX_CONFIG                    one more file, from disk or the vfs (optional)
// kAuto reads the default.ini files only: what can be known before the gpu,
// and with it the tier, is. An empty title skips the game's files. False when a
// concrete tier has no engine file, which is no config to run with.
RX_APP_EXPORT bool ReadPlatformChain(const asset::Vfs& vfs, base::StringRef title,
                                     render::QualityPreset tier, PlatformConfig* out);

// Sets each [options] entry on its registered base::Option. An option the
// environment already set keeps that value: env > file > built-in default.
// Options read at startup (window size, fullscreen) only see what the files
// read before the window opens, which is default.ini.
RX_APP_EXPORT void ApplyPlatformOptions(PlatformConfig& config);

}  // namespace rx::app

#endif  // RX_ENGINE_APP_PLATFORM_CONFIG_H_
