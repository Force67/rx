#include "importers/blend/blend_import.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <process.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif

#include "asset/asset_id.h"
#include "base/containers/vector.h"
#include "base/strings/xstring.h"
#include "core/file_system.h"
#include "core/format.h"
#include "core/log.h"

namespace rx::asset {
namespace {

base::String CacheRoot(const BlendImportOptions &options) {
  if (!options.cache_directory.empty())
    return options.cache_directory;
  if (const char *xdg = ::getenv("XDG_CACHE_HOME"))
    return fs::Join(xdg, "rx/blend");
#if defined(_WIN32)
  if (const char *local = ::getenv("LOCALAPPDATA"))
    return fs::Join(local, "rx/blend");
#else
  if (const char *home = ::getenv("HOME"))
    return fs::Join(home, ".cache/rx/blend");
#endif
  return fs::Join(fs::TempDirectory(), "rx/blend");
}

base::String CacheKey(base::StringRef source, base::StringRef script) {
  const base::Optional<u64> source_size = fs::FileSize(source);
  const base::Optional<i64> source_time = fs::LastWriteTime(source);
  const base::Optional<i64> script_time = fs::LastWriteTime(script);
  if (!source_size || !source_time || !script_time)
    return {};
  const base::String identity =
      fs::WeaklyCanonical(source) + ":" + rx::ToString(*source_size) + ":" +
      rx::ToString(*source_time) + ":" + rx::ToString(*script_time);
  char key[17];
  ::snprintf(key, sizeof(key), "%016llx",
                static_cast<unsigned long long>(MakeAssetId(identity).hash));
  return key;
}

int Run(const base::Vector<base::String> &arguments) {
  base::Vector<char *> argv;
  argv.reserve(arguments.size() + 1);
  for (const base::String &argument : arguments)
    argv.push_back(const_cast<char *>(argument.c_str()));
  argv.push_back(nullptr);
#if defined(_WIN32)
  return static_cast<int>(_spawnvp(_P_WAIT, argv[0], argv.data()));
#else
  pid_t child = 0;
  const int spawned =
      posix_spawnp(&child, argv[0], nullptr, nullptr, argv.data(), environ);
  if (spawned != 0)
    return -spawned;
  int status = 0;
  if (waitpid(child, &status, 0) < 0)
    return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

bool NonEmptyFile(base::StringRef path) {
  const base::Optional<u64> size = fs::FileSize(path);
  return size && *size > 0;
}

} // namespace

bool ConvertBlendScene(const base::String &blend_path,
                       const BlendImportOptions &options,
                       BlendImportResult *out, base::String *error) {
  if (!out)
    return false;
  *out = {};
  const base::String source = fs::Absolute(blend_path);
  const base::String script = fs::Absolute(options.converter_script);
  base::String extension(fs::Extension(source));
  for (char &c : extension)
    c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  if (extension != ".blend" || !fs::IsRegularFile(source)) {
    if (error)
      *error = "not a readable .blend file: " + source;
    return false;
  }
  if (options.converter_script.empty() || !fs::IsRegularFile(script)) {
    if (error)
      *error = "Blender converter script not found: " + script;
    return false;
  }
  const base::String key = CacheKey(source, script);
  if (key.empty()) {
    if (error)
      *error = "could not stat Blender source or converter";
    return false;
  }
  const base::String cache = fs::Join(CacheRoot(options), key);
  fs::CreateDirectories(cache);
  if (!fs::IsDirectory(cache)) {
    if (error)
      *error = base::String("could not create Blender cache: ") + ::strerror(errno);
    return false;
  }
  const base::String glb = fs::Join(cache, "scene.glb");
  const base::String manifest = fs::Join(cache, "scene.rxblend");
  if (!options.force && NonEmptyFile(glb) && fs::IsRegularFile(manifest)) {
    out->glb_path = glb;
    out->manifest_path = manifest;
    out->reused_cache = true;
    return true;
  }

  RX_INFO("blend: converting {} with {}", source, options.blender_executable);
  base::Vector<base::String> arguments;
  arguments.push_back(options.blender_executable);
  arguments.push_back("--background");
  arguments.push_back(source);
  arguments.push_back("--python");
  arguments.push_back(script);
  arguments.push_back("--");
  arguments.push_back("--output");
  arguments.push_back(glb);
  arguments.push_back("--manifest");
  arguments.push_back(manifest);
  const int result = Run(arguments);
  if (result != 0 || !NonEmptyFile(glb)) {
    fs::Remove(glb);
    fs::Remove(manifest);
    if (error)
      *error =
          "Blender conversion failed (exit " + rx::ToString(result) + ")";
    return false;
  }
  out->glb_path = glb;
  out->manifest_path = manifest;
  return true;
}

} // namespace rx::asset
