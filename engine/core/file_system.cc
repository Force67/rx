#include "core/file_system.h"

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>

#include "base/filesystem/file.h"
#include "base/filesystem/path.h"

#if defined(_WIN32)
#include <windows.h>

#include "base/text/code_convert.h"
#else
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace rx::fs {
namespace {

bool IsSeparator(char c) {
#if defined(_WIN32)
  return c == '/' || c == '\\';
#else
  return c == '/';
#endif
}

// Length of the root name ("C:" on Windows, nothing elsewhere).
mem_size RootNameLength(base::StringRef path) {
#if defined(_WIN32)
  if (path.size() >= 2 && path[1] == ':') return 2;
#endif
  (void)path;
  return 0;
}

bool HasRootDirectory(base::StringRef path) {
  const mem_size root = RootNameLength(path);
  return path.size() > root && IsSeparator(path[root]);
}

// The generic-format element sequence std::filesystem::path iterates: the
// root name, the root directory as "/", each filename, and "" for a trailing
// separator.
struct Elements {
  base::StringRef root_name;
  bool root_directory = false;
  base::Vector<base::StringRef> names;
};

Elements Split(base::StringRef path) {
  Elements e;
  const mem_size root = RootNameLength(path);
  e.root_name = path.substr(0, root);
  mem_size i = root;
  if (i < path.size() && IsSeparator(path[i])) {
    e.root_directory = true;
    while (i < path.size() && IsSeparator(path[i])) ++i;
  }
  while (i < path.size()) {
    mem_size end = i;
    while (end < path.size() && !IsSeparator(path[end])) ++end;
    e.names.push_back(path.substr(i, end - i));
    if (end == path.size()) break;
    while (end < path.size() && IsSeparator(path[end])) ++end;
    if (end == path.size()) e.names.push_back(base::StringRef());
    i = end;
  }
  return e;
}

base::StringRef View(const base::String& s) { return base::StringRef(s.data(), s.size()); }

#if defined(_WIN32)
base::StringW Wide(base::StringRef path) {
  return base::UTF8ToWide(base::StringRefU8(reinterpret_cast<const char8_t*>(path.data()),
                                            path.size()));
}

base::String Narrow(const wchar_t* text, mem_size length) {
  base::StringU8 utf8 = base::WideToUTF8(base::StringRefW(text, length));
  return base::String(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}
#else
// NUL-terminated copy for the libc calls; views are not.
base::String Z(base::StringRef path) { return base::String(path.data(), path.size()); }
#endif

base::Path ToBasePath(base::StringRef path) {
  return base::Path(
      base::StringRefU8(reinterpret_cast<const char8_t*>(path.data()), path.size()));
}

}  // namespace

bool IsAbsolute(base::StringRef path) {
#if defined(_WIN32)
  return RootNameLength(path) > 0 && HasRootDirectory(path);
#else
  return HasRootDirectory(path);
#endif
}

base::StringRef Filename(base::StringRef path) {
  const mem_size root = RootNameLength(path);
  if (path.size() == root) return base::StringRef();
  if (IsSeparator(path[path.size() - 1])) return base::StringRef();
  mem_size start = path.size();
  while (start > root && !IsSeparator(path[start - 1])) --start;
  return path.substr(start);
}

base::StringRef ParentPath(base::StringRef path) {
  const mem_size root = RootNameLength(path);
  mem_size rel = root;
  while (rel < path.size() && IsSeparator(path[rel])) ++rel;
  // Nothing after the root: the path is its own parent ("/" or "C:/"), and
  // an empty or root-name-only path has none.
  if (rel == path.size()) return path;
  // Drop the last element (a trailing separator counts as an empty one),
  // then the separators in front of it, but never the root directory.
  mem_size end = path.size();
  if (IsSeparator(path[end - 1])) {
    while (end > rel && IsSeparator(path[end - 1])) --end;
    return path.substr(0, end);
  }
  while (end > rel && !IsSeparator(path[end - 1])) --end;
  while (end > rel && IsSeparator(path[end - 1])) --end;
  if (end == rel) return path.substr(0, rel);
  return path.substr(0, end);
}

base::StringRef Extension(base::StringRef path) {
  const base::StringRef name = Filename(path);
  if (name == "." || name == "..") return base::StringRef();
  const mem_size dot = name.rfind('.');
  if (dot == base::StringRef::npos || dot == 0) return base::StringRef();
  return name.substr(dot);
}

base::StringRef Stem(base::StringRef path) {
  const base::StringRef name = Filename(path);
  const base::StringRef ext = Extension(path);
  return name.substr(0, name.size() - ext.size());
}

base::String ReplaceExtension(base::StringRef path, base::StringRef extension) {
  const base::StringRef old = Extension(path);
  base::String out(path.data(), path.size() - old.size());
  if (!extension.empty()) {
    if (extension[0] != '.') out.push_back('.');
    out.append(extension.data(), extension.size());
  }
  return out;
}

base::String Join(base::StringRef lhs, base::StringRef rhs) {
  if (IsAbsolute(rhs) || lhs.empty()) return base::String(rhs.data(), rhs.size());
  base::String out(lhs.data(), lhs.size());
  if (HasRootDirectory(rhs)) {
    // Rooted but not absolute (Windows "\a"): keep only lhs's root name.
    out = base::String(lhs.data(), RootNameLength(lhs));
  } else if (!IsSeparator(out[out.size() - 1]) && out.size() != RootNameLength(lhs)) {
    out.push_back('/');
  }
  out.append(rhs.data(), rhs.size());
  return out;
}

base::String LexicallyNormal(base::StringRef path) {
  if (path.empty()) return base::String();
  const Elements e = Split(path);
  base::Vector<base::StringRef> kept;
  bool trailing = false;
  for (const base::StringRef& name : e.names) {
    trailing = false;
    if (name.empty()) {
      trailing = true;
    } else if (name == ".") {
      trailing = true;
    } else if (name == "..") {
      if (!kept.empty() && kept.back() != "..") {
        kept.pop_back();
        trailing = true;
      } else if (!e.root_directory) {
        kept.push_back(name);
      }
    } else {
      kept.push_back(name);
    }
  }
  base::String out(e.root_name.data(), e.root_name.size());
  if (e.root_directory) out.push_back('/');
  for (mem_size i = 0; i < kept.size(); ++i) {
    if (i > 0) out.push_back('/');
    out.append(kept[i].data(), kept[i].size());
  }
  // A trailing separator survives, except after "..".
  if (trailing && !kept.empty() && kept.back() != "..") out.push_back('/');
  if (out.empty()) out = ".";
  return out;
}

base::String LexicallyRelative(base::StringRef path, base::StringRef base_dir) {
  const Elements p = Split(path);
  const Elements b = Split(base_dir);
  if (p.root_name != b.root_name || IsAbsolute(path) != IsAbsolute(base_dir) ||
      (!p.root_directory && b.root_directory)) {
    return base::String();
  }
  mem_size i = 0;
  while (i < p.names.size() && i < b.names.size() && p.names[i] == b.names[i]) ++i;
  if (i == p.names.size() && i == b.names.size()) return base::String(".");
  i64 n = 0;
  for (mem_size j = i; j < b.names.size(); ++j) {
    const base::StringRef& name = b.names[j];
    if (name == "..") {
      --n;
    } else if (!name.empty() && name != ".") {
      ++n;
    }
  }
  if (n < 0) return base::String();
  if (n == 0 && (i == p.names.size() || p.names[i].empty())) return base::String(".");
  base::String out;
  for (i64 k = 0; k < n; ++k) {
    if (!out.empty()) out.push_back('/');
    out.append("..");
  }
  for (mem_size j = i; j < p.names.size(); ++j) {
    out = Join(View(out), p.names[j]);
    // Join of "" appends a separator only when there is a filename to follow.
    if (p.names[j].empty() && !out.empty() && !IsSeparator(out[out.size() - 1])) {
      out.push_back('/');
    }
  }
  return out;
}

base::String GenericString(base::StringRef path) {
  base::String out(path.data(), path.size());
#if defined(_WIN32)
  for (char& c : out) {
    if (c == '\\') c = '/';
  }
#endif
  return out;
}

#if defined(_WIN32)

namespace {
bool Attributes(base::StringRef path, WIN32_FILE_ATTRIBUTE_DATA* data) {
  return GetFileAttributesExW(Wide(path).c_str(), GetFileExInfoStandard, data) != 0;
}
}  // namespace

bool Exists(base::StringRef path) {
  WIN32_FILE_ATTRIBUTE_DATA data;
  return Attributes(path, &data);
}

bool IsRegularFile(base::StringRef path) {
  WIN32_FILE_ATTRIBUTE_DATA data;
  return Attributes(path, &data) && !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsDirectory(base::StringRef path) {
  WIN32_FILE_ATTRIBUTE_DATA data;
  return Attributes(path, &data) && (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
}

base::Optional<u64> FileSize(base::StringRef path) {
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!Attributes(path, &data) || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
    return base::nullopt;
  }
  return (static_cast<u64>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

base::Optional<i64> LastWriteTime(base::StringRef path) {
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!Attributes(path, &data)) return base::nullopt;
  const u64 ticks = (static_cast<u64>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                    data.ftLastWriteTime.dwLowDateTime;
  // 100 ns ticks since 1601 to ns since 1970.
  constexpr u64 kEpochDelta = 116444736000000000ull;
  return static_cast<i64>(ticks - kEpochDelta) * 100;
}

base::String CurrentPath() {
  const DWORD size = GetCurrentDirectoryW(0, nullptr);
  if (size == 0) return base::String(".");
  base::Vector<wchar_t> buffer;
  buffer.resize(size);
  const DWORD written = GetCurrentDirectoryW(size, buffer.data());
  return Narrow(buffer.data(), written);
}

base::String TempDirectory() {
  wchar_t buffer[MAX_PATH + 1];
  DWORD size = GetTempPathW(MAX_PATH + 1, buffer);
  if (size == 0) return base::String(".");
  if (size > 1 && (buffer[size - 1] == L'\\' || buffer[size - 1] == L'/')) --size;
  return Narrow(buffer, size);
}

base::String WeaklyCanonical(base::StringRef path) {
  const base::StringW wide = Wide(View(Absolute(path)));
  const DWORD size = GetFullPathNameW(wide.c_str(), 0, nullptr, nullptr);
  if (size == 0) return LexicallyNormal(path);
  base::Vector<wchar_t> buffer;
  buffer.resize(size);
  const DWORD written = GetFullPathNameW(wide.c_str(), size, buffer.data(), nullptr);
  return LexicallyNormal(View(Narrow(buffer.data(), written)));
}

bool CreateDirectories(base::StringRef path) {
  if (path.empty()) return false;
  if (IsDirectory(path)) return false;
  const base::StringRef parent = ParentPath(path);
  if (!parent.empty() && parent.size() < path.size() && !IsDirectory(parent)) {
    CreateDirectories(parent);
  }
  if (Filename(path).empty()) return IsDirectory(path);
  return CreateDirectoryW(Wide(path).c_str(), nullptr) != 0;
}

bool Remove(base::StringRef path) {
  const base::StringW wide = Wide(path);
  if (IsDirectory(path)) return RemoveDirectoryW(wide.c_str()) != 0;
  return DeleteFileW(wide.c_str()) != 0;
}

bool Rename(base::StringRef from, base::StringRef to) {
  // Write-through so a rename that replaces a saved file is on disk when it
  // returns, which the terrain and texture caches' write-then-rename relies on.
  return MoveFileExW(Wide(from).c_str(), Wide(to).c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

namespace {
bool ListInto(base::StringRef dir, base::Vector<DirEntry>* out, bool recursive) {
  base::String pattern = Join(dir, "*");
  WIN32_FIND_DATAW data;
  HANDLE find = FindFirstFileW(Wide(View(pattern)).c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) return false;
  do {
    const wchar_t* name = data.cFileName;
    if (name[0] == L'.' && (name[1] == 0 || (name[1] == L'.' && name[2] == 0))) continue;
    mem_size length = 0;
    while (name[length]) ++length;
    DirEntry entry;
    entry.path = Join(dir, View(Narrow(name, length)));
    entry.is_directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    entry.is_regular = !entry.is_directory;
    const bool descend = recursive && entry.is_directory &&
                         !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
    base::String child = entry.path;
    out->push_back(base::move(entry));
    if (descend) ListInto(View(child), out, true);
  } while (FindNextFileW(find, &data));
  FindClose(find);
  return true;
}
}  // namespace

bool ListDirectory(base::StringRef dir, base::Vector<DirEntry>* out, bool recursive) {
  return ListInto(dir, out, recursive);
}

#else  // POSIX

bool Exists(base::StringRef path) {
  struct stat st;
  return ::stat(Z(path).c_str(), &st) == 0;
}

bool IsRegularFile(base::StringRef path) {
  struct stat st;
  return ::stat(Z(path).c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool IsDirectory(base::StringRef path) {
  struct stat st;
  return ::stat(Z(path).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

base::Optional<u64> FileSize(base::StringRef path) {
  struct stat st;
  if (::stat(Z(path).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return base::nullopt;
  return static_cast<u64>(st.st_size);
}

base::Optional<i64> LastWriteTime(base::StringRef path) {
  struct stat st;
  if (::stat(Z(path).c_str(), &st) != 0) return base::nullopt;
#if defined(__APPLE__)
  return static_cast<i64>(st.st_mtimespec.tv_sec) * 1000000000 + st.st_mtimespec.tv_nsec;
#else
  return static_cast<i64>(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
#endif
}

base::String CurrentPath() {
  char buffer[PATH_MAX];
  if (!::getcwd(buffer, sizeof(buffer))) return base::String(".");
  return base::String(buffer);
}

base::String TempDirectory() {
  // std::filesystem::temp_directory_path's POSIX order.
  static const char* const kVars[] = {"TMPDIR", "TMP", "TEMP", "TEMPDIR"};
  for (const char* var : kVars) {
    if (const char* dir = ::getenv(var); dir && *dir) return base::String(dir);
  }
  return base::String("/tmp");
}

base::String WeaklyCanonical(base::StringRef path) {
  // An existing path is simply canonical; otherwise resolve the longest
  // existing prefix and append the rest lexically.
  {
    char buffer[PATH_MAX];
    if (!path.empty() && ::realpath(Z(path).c_str(), buffer)) return base::String(buffer);
  }
  const Elements e = Split(path);
  base::String prefix(e.root_name.data(), e.root_name.size());
  if (e.root_directory) prefix.push_back('/');
  mem_size used = 0;
  base::String resolved;
  for (mem_size i = 0; i <= e.names.size(); ++i) {
    base::String candidate = prefix;
    for (mem_size j = 0; j < i; ++j) candidate = Join(View(candidate), e.names[j]);
    if (candidate.empty()) continue;
    char buffer[PATH_MAX];
    if (!::realpath(candidate.c_str(), buffer)) break;
    resolved = base::String(buffer);
    used = i;
  }
  if (resolved.empty()) return LexicallyNormal(path);
  for (mem_size j = used; j < e.names.size(); ++j) resolved = Join(View(resolved), e.names[j]);
  return used < e.names.size() ? LexicallyNormal(View(resolved)) : resolved;
}

bool CreateDirectories(base::StringRef path) {
  if (path.empty() || IsDirectory(path)) return false;
  const base::StringRef parent = ParentPath(path);
  if (!parent.empty() && parent.size() < path.size() && !IsDirectory(parent)) {
    CreateDirectories(parent);
  }
  if (Filename(path).empty()) return IsDirectory(path);
  return ::mkdir(Z(path).c_str(), 0777) == 0;
}

bool Remove(base::StringRef path) {
  const base::String z = Z(path);
  if (::unlink(z.c_str()) == 0) return true;
  return (errno == EISDIR || errno == EPERM) && ::rmdir(z.c_str()) == 0;
}

bool Rename(base::StringRef from, base::StringRef to) {
  return ::rename(Z(from).c_str(), Z(to).c_str()) == 0;
}

namespace {
bool ListInto(base::StringRef dir, base::Vector<DirEntry>* out, bool recursive) {
  DIR* handle = ::opendir(Z(dir).c_str());
  if (!handle) return false;
  while (const dirent* ent = ::readdir(handle)) {
    const char* name = ent->d_name;
    if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
    DirEntry entry;
    entry.path = Join(dir, name);
    struct stat st;
    // lstat: recursion does not follow directory symlinks, like
    // recursive_directory_iterator without follow_directory_symlink.
    bool is_link = false;
    if (::lstat(entry.path.c_str(), &st) == 0) {
      is_link = S_ISLNK(st.st_mode);
      if (is_link && ::stat(entry.path.c_str(), &st) != 0) st.st_mode = 0;
      entry.is_directory = S_ISDIR(st.st_mode);
      entry.is_regular = S_ISREG(st.st_mode);
    }
    const bool descend = recursive && entry.is_directory && !is_link;
    base::String child = entry.path;
    out->push_back(base::move(entry));
    if (descend) ListInto(View(child), out, true);
  }
  ::closedir(handle);
  return true;
}
}  // namespace

bool ListDirectory(base::StringRef dir, base::Vector<DirEntry>* out, bool recursive) {
  return ListInto(dir, out, recursive);
}

#endif

base::String Absolute(base::StringRef path) {
  if (IsAbsolute(path)) return base::String(path.data(), path.size());
  return Join(View(CurrentPath()), path);
}

base::String Relative(base::StringRef path, base::StringRef base_dir) {
  return LexicallyRelative(View(WeaklyCanonical(path)), View(WeaklyCanonical(base_dir)));
}

u64 RemoveAll(base::StringRef path) {
  if (!IsDirectory(path)) return Remove(path) ? 1 : 0;
  base::Vector<DirEntry> entries;
  ListDirectory(path, &entries, /*recursive=*/true);
  u64 removed = 0;
  // Children come after their directory in a pre-order listing.
  for (mem_size i = entries.size(); i > 0; --i) {
    if (Remove(View(entries[i - 1].path))) ++removed;
  }
  if (Remove(path)) ++removed;
  return removed;
}

base::File OpenFile(base::StringRef path, u32 flags) {
  return base::File(ToBasePath(path), flags);
}

namespace {
constexpr mem_size kChunk = mem_size{1} << 30;
}  // namespace

bool ReadAt(base::File& file, u64 offset, base::Span<u8> bytes) {
  for (mem_size done = 0; done < bytes.size();) {
    const mem_size n = bytes.size() - done < kChunk ? bytes.size() - done : kChunk;
    if (!file.ReadAndCheck(static_cast<i64>(offset + done), bytes.subspan(done, n))) return false;
    done += n;
  }
  return true;
}

bool WriteAll(base::File& file, base::Span<const u8> bytes) {
  for (mem_size done = 0; done < bytes.size();) {
    const mem_size n = bytes.size() - done < kChunk ? bytes.size() - done : kChunk;
    if (!file.WriteAtCurrentPosAndCheck(bytes.subspan(done, n))) return false;
    done += n;
  }
  return true;
}

bool ReadFile(base::StringRef path, base::Vector<u8>* out) {
  base::File file(ToBasePath(path), base::File::FLAG_OPEN | base::File::FLAG_READ);
  if (!file.IsValid()) return false;
  base::File::Info info;
  if (!file.GetInfo(&info) || info.is_directory) return false;
  out->resize(static_cast<mem_size>(info.size));
  mem_size done = 0;
  while (done < out->size()) {
    const mem_size chunk = out->size() - done > (1u << 30) ? (1u << 30) : out->size() - done;
    const int got = file.Read(static_cast<i64>(done), reinterpret_cast<char*>(out->data() + done),
                              static_cast<int>(chunk));
    if (got <= 0) break;
    done += static_cast<mem_size>(got);
  }
  out->resize(done);
  return done == static_cast<mem_size>(info.size);
}

bool ReadTextFile(base::StringRef path, base::String* out) {
  base::Vector<u8> bytes;
  if (!ReadFile(path, &bytes)) return false;
  *out = base::String(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return true;
}

bool WriteFile(base::StringRef path, base::Span<const u8> bytes) {
  base::File file(ToBasePath(path),
                  base::File::FLAG_CREATE_ALWAYS | base::File::FLAG_WRITE);
  if (!file.IsValid()) return false;
  return WriteAll(file, bytes);
}

bool WriteTextFile(base::StringRef path, base::StringRef text) {
  return WriteFile(path, base::Span<const u8>(reinterpret_cast<const u8*>(text.data()),
                                              text.size()));
}

}  // namespace rx::fs
