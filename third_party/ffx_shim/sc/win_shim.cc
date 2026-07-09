// Implementations for the Windows API stubs FidelityFX_SC needs on Linux,
// plus the main() -> wmain() bridge. wchar_t is UTF-32 here, so the
// WideCharToMultiByte/MultiByteToWideChar pair is a plain UTF-8 transcode.
#ifndef _WIN32

#include "Windows.h"
#include "pathcch.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

namespace {

std::string ToUtf8(const wchar_t* src, size_t len) {
  std::string out;
  out.reserve(len);
  for (size_t i = 0; i < len; ++i) {
    char32_t c = static_cast<char32_t>(src[i]);
    if (c < 0x80) {
      out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
      out.push_back(static_cast<char>(0xc0 | (c >> 6)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
    } else if (c < 0x10000) {
      out.push_back(static_cast<char>(0xe0 | (c >> 12)));
      out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
    } else {
      out.push_back(static_cast<char>(0xf0 | (c >> 18)));
      out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
    }
  }
  return out;
}

std::wstring ToWide(const char* src, size_t len) {
  std::wstring out;
  out.reserve(len);
  size_t i = 0;
  while (i < len) {
    unsigned char b = static_cast<unsigned char>(src[i]);
    char32_t c = 0;
    size_t extra = 0;
    if (b < 0x80) {
      c = b;
    } else if ((b & 0xe0) == 0xc0) {
      c = b & 0x1f;
      extra = 1;
    } else if ((b & 0xf0) == 0xe0) {
      c = b & 0x0f;
      extra = 2;
    } else {
      c = b & 0x07;
      extra = 3;
    }
    ++i;
    for (size_t j = 0; j < extra && i < len; ++j, ++i) {
      c = (c << 6) | (static_cast<unsigned char>(src[i]) & 0x3f);
    }
    out.push_back(static_cast<wchar_t>(c));
  }
  return out;
}

std::string ToUtf8(const std::wstring& s) { return ToUtf8(s.c_str(), s.size()); }

// The tool converts '/' to '\\' before calling the pathcch helpers; undo that
// so std::filesystem sees native separators.
std::wstring NormalizeSlashes(PCWSTR path) {
  std::wstring p = path ? path : L"";
  for (wchar_t& c : p) {
    if (c == L'\\') c = L'/';
  }
  return p;
}

PWSTR AllocWide(const std::wstring& s) {
  PWSTR out = static_cast<PWSTR>(std::malloc((s.size() + 1) * sizeof(wchar_t)));
  std::wmemcpy(out, s.c_str(), s.size() + 1);
  return out;
}

}  // namespace

int WideCharToMultiByte(UINT, DWORD, const wchar_t* src, int src_len, char* dst, int dst_len,
                        const char*, BOOL*) {
  size_t len = src_len < 0 ? std::wcslen(src) + 1 : static_cast<size_t>(src_len);
  std::string utf8 = ToUtf8(src, len);
  if (dst_len == 0) return static_cast<int>(utf8.size());
  if (static_cast<size_t>(dst_len) < utf8.size()) return 0;
  std::memcpy(dst, utf8.data(), utf8.size());
  return static_cast<int>(utf8.size());
}

int MultiByteToWideChar(UINT, DWORD, const char* src, int src_len, wchar_t* dst, int dst_len) {
  size_t len = src_len < 0 ? std::strlen(src) + 1 : static_cast<size_t>(src_len);
  std::wstring wide = ToWide(src, len);
  if (dst_len == 0) return static_cast<int>(wide.size());
  if (static_cast<size_t>(dst_len) < wide.size()) return 0;
  std::wmemcpy(dst, wide.data(), wide.size());
  return static_cast<int>(wide.size());
}

BOOL CreateDirectoryW(const wchar_t* path, void*) {
  std::error_code ec;
  std::filesystem::create_directories(ToUtf8(NormalizeSlashes(path)), ec);
  return ec ? 0 : 1;
}

void* LocalFree(void* mem) {
  std::free(mem);
  return nullptr;
}

int _wfopen_s(FILE** fp, const wchar_t* path, const wchar_t* mode) {
  *fp = std::fopen(ToUtf8(NormalizeSlashes(path)).c_str(), ToUtf8(mode, std::wcslen(mode)).c_str());
  return *fp ? 0 : errno;
}

HRESULT PathAllocCanonicalize(PCWSTR path, DWORD flags, PWSTR* out) {
  std::filesystem::path p =
      std::filesystem::absolute(ToUtf8(NormalizeSlashes(path))).lexically_normal();
  std::error_code ec;
  std::filesystem::create_directories(p, ec);
  std::string narrow = p.string();
  std::wstring result = ToWide(narrow.c_str(), narrow.size());
  if ((flags & PATHCCH_ENSURE_TRAILING_SLASH) && !result.empty() && result.back() != L'/') {
    result.push_back(L'/');
  }
  *out = AllocWide(result);
  return S_OK;
}

HRESULT PathCchSkipRoot(PCWSTR path, PWSTR* out) {
  *out = const_cast<PWSTR>(path) + (path && path[0] == L'/' ? 1 : 0);
  return S_OK;
}

HRESULT PathAllocCombine(PCWSTR base, PCWSTR more, DWORD, PWSTR* out) {
  std::filesystem::path combined =
      std::filesystem::path(ToUtf8(NormalizeSlashes(base))) / ToUtf8(NormalizeSlashes(more));
  std::string s = combined.lexically_normal().string();
  *out = AllocWide(ToWide(s.c_str(), s.size()));
  return S_OK;
}

int wmain(int argc, wchar_t** argv);

int main(int argc, char** argv) {
  std::vector<std::wstring> wide;
  wide.reserve(argc);
  std::vector<wchar_t*> ptrs;
  ptrs.reserve(argc + 1);
  for (int i = 0; i < argc; ++i) {
    wide.push_back(ToWide(argv[i], std::strlen(argv[i])));
    ptrs.push_back(wide.back().data());
  }
  ptrs.push_back(nullptr);
  return wmain(argc, ptrs.data());
}

#endif  // !_WIN32
