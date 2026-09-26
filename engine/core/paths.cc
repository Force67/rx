#include "core/paths.h"

#include "base/containers/array.h"
#include "base/strings/string_ref.h"
#include "core/file_system.h"
#include "core/types.h"

#if defined(_WIN32)
#include <windows.h>

#include "base/text/code_convert.h"
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace rx {

base::String ExecutableDirectory() {
#if defined(_WIN32)
  base::Array<wchar_t, 4096> path{};
  const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (size > 0 && size < path.size()) {
    const base::StringU8 utf8 = base::WideToUTF8(base::StringRefW(path.data(), size));
    const base::StringRef full(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    return fs::GenericString(fs::ParentPath(full));
  }
#elif defined(__APPLE__)
  base::Array<char, 4096> path{};
  u32 size = static_cast<u32>(path.size());
  if (_NSGetExecutablePath(path.data(), &size) == 0) {
    const base::String canonical = fs::WeaklyCanonical(path.data());
    const base::StringRef parent = fs::ParentPath(base::StringRef(canonical.data(), canonical.size()));
    return base::String(parent.data(), parent.size());
  }
#else
  base::Array<char, 4096> path{};
  const ssize_t size = readlink("/proc/self/exe", path.data(), path.size() - 1);
  if (size > 0) {
    const base::StringRef parent = fs::ParentPath(base::StringRef(path.data(), size));
    return base::String(parent.data(), parent.size());
  }
#endif
  return fs::CurrentPath();
}

}  // namespace rx
