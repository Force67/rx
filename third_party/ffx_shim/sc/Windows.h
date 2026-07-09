// Minimal Windows API surface so the FidelityFX shader compiler tool
// (FidelityFX_SC) builds unmodified on Linux. Only what ffx_sc.cpp,
// utils.cpp and the headers they pull in actually use.
#ifndef RX_FFX_SHIM_WINDOWS_H_
#define RX_FFX_SHIM_WINDOWS_H_
#ifndef _WIN32

#include <cmath>  // the tool uses ceilf/log2f/pow without including it
#include <cstdint>
#include <cstdio>

using HRESULT = long;
using BOOL = int;
using BYTE = unsigned char;
using UINT = unsigned int;
using DWORD = uint32_t;
using SIZE_T = size_t;
using LPCVOID = const void*;
using PWSTR = wchar_t*;
using PCWSTR = const wchar_t*;
using LPCWSTR = const wchar_t*;
using HMODULE = void*;
using REFIID = const void*;

constexpr HRESULT S_OK = 0;
constexpr HRESULT E_FAIL = -1;
constexpr UINT CP_UTF8 = 65001;

int WideCharToMultiByte(UINT code_page, DWORD flags, const wchar_t* src, int src_len, char* dst,
                        int dst_len, const char* default_char, BOOL* used_default);
int MultiByteToWideChar(UINT code_page, DWORD flags, const char* src, int src_len, wchar_t* dst,
                        int dst_len);
BOOL CreateDirectoryW(const wchar_t* path, void* security_attributes);
void* LocalFree(void* mem);
int _wfopen_s(FILE** fp, const wchar_t* path, const wchar_t* mode);

#endif  // !_WIN32
#endif  // RX_FFX_SHIM_WINDOWS_H_
