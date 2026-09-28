#ifndef RX_FOUNDATION_FILES_FILE_SYSTEM_H_
#define RX_FOUNDATION_FILES_FILE_SYSTEM_H_

#include "base/containers/span.h"
#include "base/containers/vector.h"
#include "base/filesystem/file.h"
#include "base/optional.h"
#include "base/strings/string_ref.h"
#include "base/strings/xstring.h"
#include "foundation/build_config/export.h"
#include "foundation/build_config/types.h"

// Files and paths for the engine, over base::File and the OS. Paths are UTF-8
// strings in generic form: '/' separates, and Windows accepts '\\' as well.
//
// The lexical functions follow std::filesystem::path on the generic format
// exactly (Filename("a/b/") is empty, LexicallyNormal("a/b/..") is "a/"),
// because asset keys and cache names are built from their output.
namespace rx::fs {

// Lexical, no disk access.
RX_FOUNDATION_EXPORT bool IsAbsolute(base::StringRef path);
RX_FOUNDATION_EXPORT base::StringRef Filename(base::StringRef path);
RX_FOUNDATION_EXPORT base::StringRef ParentPath(base::StringRef path);
RX_FOUNDATION_EXPORT base::StringRef Extension(base::StringRef path);
RX_FOUNDATION_EXPORT base::StringRef Stem(base::StringRef path);
// std::filesystem::path::replace_extension: drops Extension(path), then
// appends |extension|, with a '.' in front when it has none.
RX_FOUNDATION_EXPORT base::String ReplaceExtension(base::StringRef path, base::StringRef extension);
// std::filesystem::path::operator/: an absolute rhs replaces lhs.
RX_FOUNDATION_EXPORT base::String Join(base::StringRef lhs, base::StringRef rhs);
RX_FOUNDATION_EXPORT base::String LexicallyNormal(base::StringRef path);
RX_FOUNDATION_EXPORT base::String LexicallyRelative(base::StringRef path, base::StringRef base_dir);
// '\\' to '/' on Windows, the input elsewhere.
RX_FOUNDATION_EXPORT base::String GenericString(base::StringRef path);

// Disk queries. Each is false/empty when the path cannot be stat'ed.
RX_FOUNDATION_EXPORT bool Exists(base::StringRef path);
RX_FOUNDATION_EXPORT bool IsRegularFile(base::StringRef path);
RX_FOUNDATION_EXPORT bool IsDirectory(base::StringRef path);
RX_FOUNDATION_EXPORT base::Optional<u64> FileSize(base::StringRef path);
// Nanoseconds since the Unix epoch. Only meaningful compared to itself.
RX_FOUNDATION_EXPORT base::Optional<i64> LastWriteTime(base::StringRef path);

RX_FOUNDATION_EXPORT base::String CurrentPath();
RX_FOUNDATION_EXPORT base::String TempDirectory();
RX_FOUNDATION_EXPORT base::String Absolute(base::StringRef path);
// The existing prefix resolved through symlinks, the rest appended lexically.
RX_FOUNDATION_EXPORT base::String WeaklyCanonical(base::StringRef path);
// LexicallyRelative of both paths after WeaklyCanonical.
RX_FOUNDATION_EXPORT base::String Relative(base::StringRef path, base::StringRef base_dir);

// Mutation. Each returns false on failure; errno/GetLastError tell why.
RX_FOUNDATION_EXPORT bool CreateDirectories(base::StringRef path);
// A file or an empty directory. False also when nothing was there.
RX_FOUNDATION_EXPORT bool Remove(base::StringRef path);
// Everything under path, and path. Returns how many entries went.
RX_FOUNDATION_EXPORT u64 RemoveAll(base::StringRef path);
// Replaces an existing destination file, as std::filesystem::rename does.
RX_FOUNDATION_EXPORT bool Rename(base::StringRef from, base::StringRef to);

struct DirEntry {
  base::String path;  // the directory argument joined with the entry name
  bool is_directory = false;
  bool is_regular = false;
};
// Entries in readdir / FindNextFile order, which is the order
// std::filesystem::directory_iterator walks; "." and ".." are skipped. A
// recursive listing is a pre-order walk that descends into each directory as
// it is met, like recursive_directory_iterator. False if dir cannot be opened.
RX_FOUNDATION_EXPORT bool ListDirectory(base::StringRef dir, base::Vector<DirEntry>* out,
                                  bool recursive = false);

// base::File on a UTF-8 path; flags are base::File::FLAG_*.
RX_FOUNDATION_EXPORT base::File OpenFile(base::StringRef path, u32 flags);
// All of |bytes|, in the int-sized pieces base::File takes. False on a short
// read or write.
RX_FOUNDATION_EXPORT bool ReadAt(base::File& file, u64 offset, base::Span<u8> bytes);
RX_FOUNDATION_EXPORT bool WriteAll(base::File& file, base::Span<const u8> bytes);

// Whole files. Read fails on a missing or unreadable file; Write truncates.
RX_FOUNDATION_EXPORT bool ReadFile(base::StringRef path, base::Vector<u8>* out);
RX_FOUNDATION_EXPORT bool ReadTextFile(base::StringRef path, base::String* out);
RX_FOUNDATION_EXPORT bool WriteFile(base::StringRef path, base::Span<const u8> bytes);
RX_FOUNDATION_EXPORT bool WriteTextFile(base::StringRef path, base::StringRef text);

}  // namespace rx::fs

#endif  // RX_FOUNDATION_FILES_FILE_SYSTEM_H_
