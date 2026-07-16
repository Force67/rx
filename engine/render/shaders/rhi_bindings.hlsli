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
//
// RX_RAY_MASK_APPROX tags the "opaque approximation" of alpha-masked
// vegetation (AC Shadows technique): a duplicate of each masked mesh whose
// triangles are shrunk about their centroid by the baked average opacity and
// flagged OPAQUE. Realtime diffuse GI / AO / shadow rays trace
// RX_RAY_MASK_DIFFUSE (realtime + approx) with RAY_FLAG_CULL_NON_OPAQUE, so
// they hit the shrunk opaque stand-in and skip the real (non-opaque) masked
// geometry entirely -- correct-on-average foliage occlusion at ~60% cost.
// The path tracer (PATHTRACE) and specular reflections (REALTIME, via a
// bounded any-hit alpha test) still see only the real masked geometry; the
// approx instance carries neither bit, so no ray ever sees both variants.
#define RX_RAY_MASK_REALTIME 0x01
#define RX_RAY_MASK_PATHTRACE 0x02
#define RX_RAY_MASK_APPROX 0x04
#define RX_RAY_MASK_DIFFUSE (RX_RAY_MASK_REALTIME | RX_RAY_MASK_APPROX)
