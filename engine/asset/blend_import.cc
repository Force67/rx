#include "asset/blend_import.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif

#include "asset/asset_id.h"
#include "core/log.h"

namespace rx::asset {
namespace fs = std::filesystem;
namespace {

std::string CacheRoot(const BlendImportOptions &options) {
  if (!options.cache_directory.empty())
    return options.cache_directory;
  if (const char *xdg = std::getenv("XDG_CACHE_HOME"))
    return (fs::path(xdg) / "rx/blend").string();
#if defined(_WIN32)
  if (const char *local = std::getenv("LOCALAPPDATA"))
    return (fs::path(local) / "rx/blend").string();
#else
  if (const char *home = std::getenv("HOME"))
    return (fs::path(home) / ".cache/rx/blend").string();
#endif
  return (fs::temp_directory_path() / "rx/blend").string();
}

std::string CacheKey(const fs::path &source, const fs::path &script) {
  std::error_code error;
  const auto source_size = fs::file_size(source, error);
  if (error)
    return {};
  const auto source_time =
      fs::last_write_time(source, error).time_since_epoch().count();
  if (error)
    return {};
  const auto script_time =
      fs::last_write_time(script, error).time_since_epoch().count();
  if (error)
    return {};
  const std::string identity = fs::weakly_canonical(source).string() + ":" +
                               std::to_string(source_size) + ":" +
                               std::to_string(source_time) + ":" +
                               std::to_string(script_time);
  char key[17];
  std::snprintf(key, sizeof(key), "%016llx",
                static_cast<unsigned long long>(MakeAssetId(identity).hash));
  return key;
}

int Run(const std::vector<std::string> &arguments) {
  std::vector<char *> argv;
  argv.reserve(arguments.size() + 1);
  for (const std::string &argument : arguments)
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

} // namespace

bool ConvertBlendScene(const std::string &blend_path,
                       const BlendImportOptions &options,
                       BlendImportResult *out, std::string *error) {
  if (!out)
    return false;
  *out = {};
  const fs::path source = fs::absolute(blend_path);
  const fs::path script = fs::absolute(options.converter_script);
  std::string extension = source.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (extension != ".blend" || !fs::is_regular_file(source)) {
    if (error)
      *error = "not a readable .blend file: " + source.string();
    return false;
  }
  if (options.converter_script.empty() || !fs::is_regular_file(script)) {
    if (error)
      *error = "Blender converter script not found: " + script.string();
    return false;
  }
  const std::string key = CacheKey(source, script);
  if (key.empty()) {
    if (error)
      *error = "could not stat Blender source or converter";
    return false;
  }
  std::error_code fs_error;
  const fs::path cache = fs::path(CacheRoot(options)) / key;
  fs::create_directories(cache, fs_error);
  if (fs_error) {
    if (error)
      *error = "could not create Blender cache: " + fs_error.message();
    return false;
  }
  const fs::path glb = cache / "scene.glb";
  const fs::path manifest = cache / "scene.rxblend";
  if (!options.force && fs::is_regular_file(glb) &&
      fs::file_size(glb, fs_error) > 0 && fs::is_regular_file(manifest)) {
    out->glb_path = glb.string();
    out->manifest_path = manifest.string();
    out->reused_cache = true;
    return true;
  }

  RX_INFO("blend: converting {} with {}", source.string(),
          options.blender_executable);
  const std::vector<std::string> arguments = {
      options.blender_executable,
      "--background",
      source.string(),
      "--python",
      script.string(),
      "--",
      "--output",
      glb.string(),
      "--manifest",
      manifest.string(),
  };
  const int result = Run(arguments);
  if (result != 0 || !fs::is_regular_file(glb) ||
      fs::file_size(glb, fs_error) == 0) {
    fs::remove(glb, fs_error);
    fs::remove(manifest, fs_error);
    if (error)
      *error =
          "Blender conversion failed (exit " + std::to_string(result) + ")";
    return false;
  }
  out->glb_path = glb.string();
  out->manifest_path = manifest.string();
  return true;
}

} // namespace rx::asset
