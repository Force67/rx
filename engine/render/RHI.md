# Render Hardware Interface (RHI)

The renderer is written against the backend-agnostic API in `render/rhi/` -
no pass, system or public header may name a Vulkan/D3D12/console type. Each
backend lives in its own directory and translates:

```
render/rhi/        public API: types.h resources.h bindings.h pipeline.h
                   command_list.h device.h swapchain.h  (+ vulkan_interop.h)
render/vulkan/     Vulkan 1.3 backend (dynamic rendering, sync2, BDA baseline)
render/d3d12/      D3D12 backend (windows d3d12.dll / linux vkd3d; see below)
render/null/       no-op backend: headless/serverside + interface conformance
```

Backend selection: `DeviceDesc::backend` (`kAuto` → platform native, then
Vulkan, then null). Build flags `RX_RHI_VULKAN` / `RX_RHI_D3D12`
gate compilation; the null backend always builds.

## Core model

- **Device** (`rhi/device.h`): resource + pipeline creation, frame ring
  (`BeginFrame(slot)` / `SubmitFrame`), `ImmediateSubmit` for uploads. Owns all
  sync primitives - nothing above the RHI touches fences or semaphores.
- **CommandList** (`rhi/command_list.h`): recording. Dynamic-rendering-style
  raster (`BeginRendering` auto-sets viewport/scissor), compute, transfers,
  acceleration-structure builds, timestamps, debug labels.
- **Resources** (`rhi/resources.h`): `GpuBuffer`/`GpuImage` are value types
  holding an opaque backend handle plus mirrored metadata (`size`, `mapped`,
  `address`, `format`, `extent`). Copies alias; destroy exactly once via the
  Device.
- **Pipelines** (`rhi/pipeline.h`): descriptor-driven. A pass declares its
  binding slots inline in the desc; the backend derives and caches set/pipeline
  layouts. Shared sets (bindless, frame globals) pass a `BindingLayoutHandle`.
- **Bindings** (`rhi/bindings.h`): transient per-record sets via
  `cmd->BindTransient(set, {Bind::Storage(0, img), ...})` (replaces the
  allocate/update/bind descriptor dance); persistent sets via
  `Device::CreateBindingSet` + `UpdateBindingSet` (bindless registry).
- **States** (`rhi/types.h`): images move between coarse `ResourceState`s
  (`kGeneral`, `kShaderRead*`, `kColorTarget`, ...). The render graph derives
  inter-pass barriers; manual transitions use
  `cmd->Barrier(Transition(img, before, after))`. Buffer hazards use coarse
  scopes: `cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kIndirectArgs)`.
- **Shaders**: HLSL compiled by dxc, dual-target: SM 6.6 SPIR-V always, plus
  an SM 6.5 DXIL sidecar when the d3d12 backend is built (see the D3D12
  section). `RX_SHADER(k_foo_cs_hlsl)` wraps both as one `ShaderBlob`; pass
  code does not change per backend.
- **Samplers**: `device.GetSampler(SamplerDesc{...})` - cached, never destroyed
  by callers.
- **Ray tracing**: ray queries only (no RT pipelines/SBT). `AccelTriangles`,
  `TlasInstance` (layout-identical between VK and D3D12) and
  `Device::CreateAccelStruct`/`cmd->BuildBlas/BuildTlas`. `RayTracingContext`
  (`gi/raytracing.*`) owns BLAS/TLAS lifecycles on top.
- **AS compaction**: build with `BlasBuildDesc::allow_compaction` (set it on the
  matching `GetBlasSizes` too), record `cmd->QueryCompactedSizes(query, ...)`
  after the builds, then read back with `Device::GetCompactedSizes` once the
  submission's fence has signalled (non-blocking, poll a frame later or read
  straight after `ImmediateSubmit`). Create a tight AS at the reported size and
  `cmd->CopyAccelStruct(dst, src, /*compact=*/true)` into it; retire the fat one
  via `DestroyAccelStructDeferred`. Vulkan-complete; d3d12 implements the size
  query via `EmitRaytracingAccelerationStructurePostbuildInfo` + readback copy
  (needs real DXR - under vkd3d, which reports no raytracing caps, the query
  handle comes back null and callers skip compaction; Windows runtime
  validation pending); null inert. The engine `RayTracingContext` compacts
  every mesh BLAS on upload whenever the query is available.

## Migration pattern (old raw Vulkan → RHI)

| old | new |
|---|---|
| `VkDescriptorSetLayout` + `VkPipelineLayout` + `VkPipeline` members | one `PipelineHandle` |
| `MakeSetLayout` / `vkCreate*Layout` / `vkCreateComputePipelines` | `device.CreateComputePipeline({.shader, .sets, .push_constant_size, .debug_name})` |
| `vkCreateGraphicsPipelines` + `VkPipelineRenderingCreateInfo` | `device.CreateGraphicsPipeline(...)` with `color_formats`/`depth`/`blend` presets |
| `ctx.allocate_set` + `VkWriteDescriptorSet[]` + `vkUpdateDescriptorSets` + `vkCmdBindDescriptorSets` | `ctx.cmd->BindTransient(set_index, {Bind::...})` after `BindPipeline` |
| `vkCmdBindPipeline` / `vkCmdPushConstants` / `vkCmdDispatch` | `cmd->BindPipeline` / `cmd->Push(pc)` / `cmd->Dispatch2D(extent)` |
| `vkCmdBeginRendering` + viewport/scissor | `cmd->BeginRendering({extent, colors, &depth})` |
| `vkCmdPipelineBarrier2` image barrier | `cmd->Barrier(Transition(image, before, after))` |
| `vkCmdPipelineBarrier2` memory barrier | `cmd->MemoryBarrier(src_scope, dst_scope)` |
| `VkFormat` / `VkExtent2D` / `VkImageLayout` | `Format` / `Extent2D` / `ResourceState` |
| `VK_IMAGE_USAGE_*` / `VK_BUFFER_USAGE_*` | `kTextureUsage*` / `kBufferUsage*` |
| `vkCreateSampler` per pass | `device.GetSampler({...})` |
| `vkGetBufferDeviceAddress` | `buffer.address` (created with `kBufferUsageDeviceAddress`) |
| `VkAccelerationStructureKHR` param | `AccelStructHandle` (`Bind::Accel(slot, tlas)`) |

Exemplars: `screenspace/ssao.*` (compute), `gi/recon_path_tracer.*` (ray
query + history imports + shared bindless set), `gi/raytracing.*` (AS builds),
`core/bindless.*` (persistent update-after-bind set).

## Interop escape hatch

Modules that integrate API-specific SDKs - NRD, DLSS, FSR3, the runtime gui
backend, the thumbnailer - include `rhi/vulkan_interop.h` (guarded by
`RX_RHI_VULKAN`) and pull raw handles via `GetVulkanHandles(device)`,
`GetVkCommandBuffer(cmd)`, `GetVkImage/GetVkImageView/...`. This keeps them
fully functional on the Vulkan backend without leaking Vulkan into the
portable surface. On other backends they must degrade gracefully (feature
unavailable), which the existing option plumbing already handles.

## D3D12 backend

Implemented in `d3d12/` (device, command list, offscreen/DXGI swapchain,
convert tables) and validated end to end on Linux by running the whole engine
over vkd3d (WineHQ vkd3d 2.0, the native D3D12-on-Vulkan library, provided by
the nix dev shell). `vkrun env RX_RHI=d3d12 ./build/nix/runtime/rx
--demo materials --no-rt` renders the materials demo pixel-identical to the
Vulkan backend outside the stochastic cloud layer (MAE < 1/255, p99 = 0
against `RX_RHI=vulkan` with clouds pinned off; the demo is not run-to-run
deterministic - animated particles and TAA state add temporal noise to any
two captures, so judge parity against a same-backend rerun baseline).

Offscreen devices are wired for d3d12 (`Device::CreateOffscreen` with
`Backend::kD3D12`): no swapchain, frames complete through the swapchainless
`SubmitFrame(CommandList*)`, and pixels come back via `ReadbackImage`
(READBACK-heap staging; `ImmediateSubmit`'s fence wait covers host
visibility). The `offscreen_test`/`compaction_test` acceptance tests pick
their backend from `RX_RHI` and are registered twice (`*_d3d12` variants)
when `RX_RHI_D3D12` is on. Validation status under
`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`: the Vulkan backend and the
d3d12 offscreen tests run clean; the full demo on d3d12 still reports a
couple dozen errors that originate inside vkd3d 2.0's own translation
(read-only-depth render pass layouts, typeless-depth
`VkImageFormatListCreateInfo` gaps), not from rx's D3D12 API usage - the
real arbiter for that half is the D3D12 debug layer on a Windows runtime.

### Wine runtime (real d3d12.dll)

The `_WIN32` half of the backend also runs against Wine's own `d3d12.dll`
(a Windows-shaped runtime rather than in-process vkd3d): the tests
cross-compile as x86_64 PEs via `cmake/toolchain-mingw-w64.cmake` and run
through `tools/wine_d3d12_test.sh` - on aarch64 hosts the x86_64 Wine runs
under box64, whose winevulkan -> libvulkan boundary is wrapped to the native
loader, so the host NVIDIA ICD *and* the Khronos validation layer serve the
emulated process. Verified on the GB10 box: `offscreen_test` renders
pixel-exact and both tests report zero validation errors on the Vulkan
instance underneath Wine's d3d12; `compaction_test` skips (Wine's bundled
vkd3d reports no DXR tier here). wined3d must be pinned to its Vulkan
adapter path (`HKCU\Software\Wine\Direct3D renderer=vulkan`, the script does
this) - the default GL path finds no pixel formats under Xvfb. The DXGI
flip-model swapchain still needs a real interactive Windows desktop to
validate; the mingw cross build required no source changes in the backend
itself beyond loading SDL3's window-property helpers dynamically.

### How it maps

- **Shaders**: every HLSL resource carries both `[[vk::binding(N, S)]]` and
  `: register(<class>N, spaceS)`; push blocks use `PUSH_CONSTANTS(T, name)`
  from `shaders/rhi_bindings.hlsli` (SPIR-V push constants / DXIL cbuffer at
  `b999, space0`). The build embeds a DXIL sidecar (`k_<sym>_dxil`) next to
  the SPIR-V when `RX_RHI_D3D12` is on; the sidecar targets SM 6.5
  (vkd3d 2.0's ceiling - 6.6 DXIL is rejected; 6.5 still covers ray queries
  and mesh shaders). The DXIL is unsigned, which vkd3d accepts natively and
  Windows accepts with experimental shader models; shipping Windows builds
  would sign via dxil.dll. The ray-hit shading readers are dual-path through
  `shaders/rt_geometry.hlsli`: `vk::RawBufferLoad` on buffer device
  addresses for SPIR-V, a bindless `ByteAddressBuffer` array
  (`BindingType::kByteBuffer`, one contiguous SRV range at `t4100` in the
  bindless space) for DXIL, with `MeshRecord` carrying both the addresses
  and the array slots. Only the mesh-shader path (`mesh_scene.as/ms`,
  `meshlet.ms`) remains on the `RX_SHADER_NO_DXIL` list with a null
  sidecar; its pipelines report unavailable and the passes disable
  themselves. DXIL links inter-stage IO by register, so a pixel shader must
  declare the same leading inputs its vertex shader outputs (shadow.ps
  declares SV_Position for this; vkd3d-proton validates the match, WineHQ
  vkd3d 2.0 silently accepted mismatches).
- **Bindings**: one root-signature descriptor table per set for views and one
  for samplers (`space = set index`); `kStorageBuffer` slots occupy an SRV+UAV
  descriptor pair (raw views - vkd3d lowers structured access to byte ranges;
  descriptor-stride-consuming Windows hardware needs the rhi to carry element
  strides, see below). Transient sets write straight into a per-frame window
  of the shader-visible heap; persistent sets (bindless registry, material
  sets) get reserved regions with null-initialized descriptors. Sampler
  tables are content-hashed and cached forever (2048-descriptor api cap).
- **Push constants**: ≤ 64 B ride as root constants; larger blocks spill into
  a per-frame upload ring behind a root CBV, with a CPU shadow so offset
  pushes (shadow cascade matrix at 0, per-draw model at 64) keep Vulkan
  semantics.
- **Skinned meshes (RX_BDA convention)**: the skinned vertex shaders read
  the bone palette through `vk::RawBufferLoad` on SPIR-V and through a
  `ByteAddressBuffer` root SRV at `(t998, space0)` on DXIL. The backend
  detects skinned pipelines by their `BLENDINDICES` input signature and binds
  that root SRV from the u64 palette address it finds at byte 128 of the push
  block (both `mesh.vs` and `shadow.vs` keep it there). Morph target deltas
  and weights extend the convention with root SRVs at `(t997/t996, space0)`,
  fed from the addresses at push bytes 160/168 of the 192-byte mesh push
  block (`MeshPushConstants`); other push sizes leave them unbound.
- **Barriers**: legacy `D3D12_RESOURCE_STATES` (vkd3d 2.0 has no enhanced
  barriers). Texture states are tracked per subresource on the device record;
  buffers decay to COMMON per ExecuteCommandLists, so per-list lazy
  transitions cover copies, index/vertex/indirect/CBV/UAV use. The coarse rhi
  `MemoryBarrier` scopes lower to a global UAV barrier.
- **Orientation**: the backend records inverted viewports (negative height,
  legal in D3D12), which cancels the D3D/Vulkan NDC y-flip so the shared
  Vulkan-tuned HLSL and matrices render identically on both backends.
- **Swapchain**: DXGI flip-model on Windows (compile-guarded); on Linux an
  offscreen ring of three render targets - no display path exists through
  vkd3d, "present" is a no-op and `RX_UI_SHOT` reads frames back through the
  normal `CopyTextureToBuffer` path (with the 256-byte row-pitch staging
  shuffle D3D12 requires).
- **Copies**: committed resources only for now (suballocation via D3D12MA is
  a TODO); `BlitMip` lowers to a fullscreen draw (`shaders/util/blit.ps`),
  `FillBuffer`/`ClearColor` to UAV/RTV clears.
- **Ray tracing & mesh shaders**: prebuild info, AS creation and
  BLAS/TLAS builds are implemented against `ID3D12Device5` /
  `ID3D12GraphicsCommandList4`, mesh PSOs against `ID3D12Device2` state
  streams - all caps-gated. vkd3d 2.0 reports neither
  (`raytracing/ray_query/mesh_shaders = false`), so on Linux these paths
  compile but stay unreached; the engine's existing gating handles it.

### Running the d3d12 backend on Linux

```
nix develop            # dev shell provides vkd3d 2.0 (libvkd3d, libvkd3d-utils)
cmake -DRX_RHI_D3D12=ON build/nix   # defaults ON when vkd3d is found
ninja -C build/nix rx
vkrun env RX_RHI=d3d12 RX_UI_SHOT=/tmp/shot.png RX_UI_SHOT_FRAMES=45 \
  ./build/nix/runtime/rx --demo materials --no-rt
```

vkd3d findings (2026-07): unsigned DXIL is accepted; the highest usable
shader model is 6.5 (`HighestShaderModel` reports 6.0, but 6.1-6.5 DXIL
creates fine and 6.6 fails); `WIDL_EXPLICIT_AGGREGATE_RETURNS` is required -
libvkd3d implements aggregate returns with the Windows out-pointer ABI.

### Windows-pending

- Runtime validation of the whole backend on real d3d12.dll (only vkd3d has
  executed it), including the DXGI swapchain, negative-viewport behaviour and
  the unsigned-DXIL experimental-features toggle.
- DXR and mesh-shader runtime validation (code paths exist, no capable
  device here reports the caps).
- Structured-buffer element strides: descriptors are raw views today, which
  vkd3d lowers correctly; hardware that indexes via the descriptor stride
  needs `BindingItem` to carry strides.
- Vulkan-interop consumers (ugui HUD/menus, imgui debug overlay, NRD, DLSS,
  FSR3, thumbnailer) degrade gracefully on d3d12 - feature-unavailable, as
  designed. A d3d12 gui backend would restore UI there.
- The mesh-shader shaders (`mesh_scene.as/ms` push-constant BDA reads,
  `meshlet.ms` multi-branch SetMeshOutputCounts) still need DXIL-compatible
  ports; the ray-hit readers are already dual-path and run on DXR (verified
  through vkd3d-proton, `RX_VKD3D_PROTON=ON`: RT reflections/DDGI/path
  tracer render on the d3d12 backend on Linux).

Console backends follow the same recipe in their own directory; nothing in
pass code assumes descriptor sets, image layouts or SPIR-V.

## HDR presentation

`Device::CreateSwapchain(..., hdr)` requests an HDR surface: HDR10 PQ
(A2B10G10R10 + ST2084) preferred, scRGB (RGBA16F + extended-sRGB linear)
second, silent SDR fallback otherwise - `Swapchain::color_space()` reports
the negotiated space and the tonemap pass encodes accordingly (sRGB / PQ /
scRGB). Enable with `RX_HDR_OUTPUT=1`; `RX_HDR_PAPER_WHITE=<nits>` maps
tonemapped white (default 200). The HDR modes are SDR-referred v1: grading
and LUTs stay valid, highlights are not carried past the tonemapper yet.
`RX_HDR_FORCE_TRANSFER=1|2` forces the encode on an SDR surface for
numeric testing (verified: PQ and scRGB round-trip at the container's
quantization floor).

Pending: a display with an HDR-capable surface (this box's X11 session
offers only SRGB_NONLINEAR - Wayland or Windows needed for runtime
validation), HDR10 metadata (VK_EXT_hdr_metadata), and the UI caveat: gui /
HUD passes draw after the tonemap in sRGB values, so on a real HDR surface
they will render dim/miscoded until they encode per color_space() too.
