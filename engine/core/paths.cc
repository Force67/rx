#include "core/paths.h"

#include <array>

#include "core/types.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace rx {

std::filesystem::path ExecutableDirectory() {
#if defined(_WIN32)
  std::array<wchar_t, 4096> path{};
  const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (size > 0 && size < path.size())
    return std::filesystem::path(std::wstring_view(path.data(), size)).parent_path();
#elif defined(__APPLE__)
  std::array<char, 4096> path{};
  u32 size = static_cast<u32>(path.size());
  if (_NSGetExecutablePath(path.data(), &size) == 0)
    return std::filesystem::weakly_canonical(path.data()).parent_path();
#else
  std::array<char, 4096> path{};
  const ssize_t size = readlink("/proc/self/exe", path.data(), path.size() - 1);
  if (size > 0) return std::filesystem::path(std::string_view(path.data(), size)).parent_path();
#endif
  std::error_code ec;
  std::filesystem::path cwd = std::filesystem::current_path(ec);
  return ec ? std::filesystem::path(".") : cwd;
}

}  // namespace rx
