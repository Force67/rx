// Dual-target binding helpers. The engine's HLSL compiles to SPIR-V (Vulkan)
// and DXIL (D3D12) from the same source: [[vk::binding(N, S)]] drives the
// SPIR-V descriptor assignment, the sibling `: register(<class>N, spaceS)`
// annotation drives DXIL. Push-constant blocks have no DXIL equivalent, so
// PUSH_CONSTANTS lowers to a ConstantBuffer in register(b999, space0) that the
// d3d12 backend feeds through root constants (or a root CBV ring for blocks
// over the root-signature budget). dxc defines __spirv__ only when -spirv is
// passed, which keeps the Vulkan path byte-identical.
#ifndef RX_RHI_BINDINGS_HLSLI_
#define RX_RHI_BINDINGS_HLSLI_

#ifdef __spirv__
#define PUSH_CONSTANTS(T, name) [[vk::push_constant]] T name
#else
#define PUSH_CONSTANTS(T, name) ConstantBuffer<T> name : register(b999, space0)
#endif

#endif  // RX_RHI_BINDINGS_HLSLI_

// TLAS instance masks; mirror render::RayMask (gi/raytracing.h). Realtime
// effects trace RX_RAY_MASK_REALTIME and never see no_rt fill geometry; the
// path-tracer family traces RX_RAY_MASK_PATHTRACE, which includes it.
#define RX_RAY_MASK_REALTIME 0x01
#define RX_RAY_MASK_PATHTRACE 0x02
