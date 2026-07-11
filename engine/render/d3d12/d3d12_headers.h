#ifndef RX_RENDER_D3D12_D3D12_HEADERS_H_
#define RX_RENDER_D3D12_D3D12_HEADERS_H_

// D3D12 API include shim. On Windows the real SDK headers apply; everywhere
// else vkd3d's headers provide the identical API surface natively
// (vkd3d_windows.h supplies the COM/Windows base types, vkd3d_d3d12.h the API,
// vkd3d_utils.h D3D12CreateDevice/D3D12SerializeRootSignature and the event
// helpers). Include this before any engine header that uses std::min/max:
// vkd3d_windows.h defines min/max macros which are undone below.

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX  // engine headers use std::min/max
#endif

#include <d3d12.h>
#include <dxgi1_6.h>

namespace rx::render::d3d12 {
inline HANDLE CreateFenceEvent() { return CreateEventA(nullptr, FALSE, FALSE, nullptr); }
inline void WaitFenceEvent(HANDLE event) { WaitForSingleObject(event, INFINITE); }
inline void DestroyFenceEvent(HANDLE event) { CloseHandle(event); }
}  // namespace rx::render::d3d12

#elif defined(RX_VKD3D_PROTON)  // vkd3d-proton native (Linux)

// Proton's D3D12: same widl-generated headers and aggregate-return ABI as
// WineHQ vkd3d (see below), but D3D12CreateDevice & friends export straight
// from libvkd3d-proton-d3d12.so and fence events are eventfd file
// descriptors (ID3D12Fence::SetEventOnCompletion writes the fd), matching
// the native demos in the vkd3d-proton tree.
#define WIDL_EXPLICIT_AGGREGATE_RETURNS

#include <vkd3d_windows.h>
#include <vkd3d_d3d12.h>

#include <sys/eventfd.h>
#include <unistd.h>

#include <cstdint>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace rx::render::d3d12 {
inline HANDLE CreateFenceEvent() {
  return reinterpret_cast<HANDLE>(static_cast<intptr_t>(eventfd(0, EFD_CLOEXEC)));
}
inline void WaitFenceEvent(HANDLE event) {
  uint64_t value = 0;
  ssize_t r = read(static_cast<int>(reinterpret_cast<intptr_t>(event)), &value, sizeof(value));
  (void)r;
}
inline void DestroyFenceEvent(HANDLE event) {
  close(static_cast<int>(reinterpret_cast<intptr_t>(event)));
}
}  // namespace rx::render::d3d12

#else  // WineHQ vkd3d (Linux and friends)

// libvkd3d implements aggregate-returning methods (GetDesc,
// Get*DescriptorHandleForHeapStart, GetResourceAllocationInfo, ...) with the
// Windows ABI: the aggregate returns through an explicit out pointer. This
// macro makes the C++ headers declare that ABI (plus source-compatible inline
// wrappers); without it the SysV small-struct return convention reads a
// garbage out-pointer and crashes inside libvkd3d.
#define WIDL_EXPLICIT_AGGREGATE_RETURNS

#include <vkd3d_utils.h>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace rx::render::d3d12 {
inline HANDLE CreateFenceEvent() { return vkd3d_create_event(); }
inline void WaitFenceEvent(HANDLE event) { vkd3d_wait_event(event, VKD3D_INFINITE); }
inline void DestroyFenceEvent(HANDLE event) { vkd3d_destroy_event(event); }
}  // namespace rx::render::d3d12

#endif

#endif  // RX_RENDER_D3D12_D3D12_HEADERS_H_
