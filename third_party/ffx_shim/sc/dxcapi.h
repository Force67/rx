// DXC API stub, see atlcomcli.h. HLSL compilation is Windows-only.
#ifndef RX_FFX_SHIM_DXCAPI_H_
#define RX_FFX_SHIM_DXCAPI_H_
#ifndef _WIN32

struct IDxcBlob;
struct IDxcResult;
struct IDxcUtils;
struct IDxcCompiler3;
struct IDxcIncludeHandler;
using DxcCreateInstanceProc = void*;

#endif  // !_WIN32
#endif  // RX_FFX_SHIM_DXCAPI_H_
