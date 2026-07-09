// D3DCompiler stub, see atlcomcli.h. The FXC path is Windows-only.
#ifndef RX_FFX_SHIM_D3DCOMPILER_H_
#define RX_FFX_SHIM_D3DCOMPILER_H_
#ifndef _WIN32

#include "Windows.h"

struct ID3DBlob;
enum D3D_BLOB_PART : int {};
using pD3DCompile = void*;

#endif  // !_WIN32
#endif  // RX_FFX_SHIM_D3DCOMPILER_H_
