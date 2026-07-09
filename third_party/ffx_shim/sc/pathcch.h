// Linux stand-ins for the pathcch.h helpers FidelityFX_SC uses. Paths come
// through with backslashes (the tool normalizes to '\\'); the implementations
// convert back to '/' and create directories as a side effect, so the tool's
// component-by-component CreateDirectoryW loop becomes a no-op.
#ifndef RX_FFX_SHIM_PATHCCH_H_
#define RX_FFX_SHIM_PATHCCH_H_
#ifndef _WIN32

#include "Windows.h"

constexpr DWORD PATHCCH_ALLOW_LONG_PATHS = 0x1;
constexpr DWORD PATHCCH_ENSURE_TRAILING_SLASH = 0x2;

HRESULT PathAllocCanonicalize(PCWSTR path, DWORD flags, PWSTR* out);
HRESULT PathCchSkipRoot(PCWSTR path, PWSTR* out);
HRESULT PathAllocCombine(PCWSTR base, PCWSTR more, DWORD flags, PWSTR* out);

#endif  // !_WIN32
#endif  // RX_FFX_SHIM_PATHCCH_H_
