# Slang shaders

Shaders can be written in [Slang](https://shader-slang.org/) alongside HLSL.
Both feed the same `rx_embed_shaders` flow (cmake/shaders.cmake): the stage
comes from the file suffix — `<name>.{vs,ps,cs,ms,as}.{hlsl,slang}` — the
entry point is always `main`, and the result embeds as the same C array
(`k_blit_ps_slang` for `blit.ps.slang`) consumed through `RX_SHADER`. Nothing
downstream of the embed step knows or cares which language a blob came from.

## Toolchain

| target | hlsl | slang |
|---|---|---|
| SPIR-V (vulkan 1.3) | `dxc -spirv -fspv-target-env=vulkan1.3 -T <stage>_6_6` | `slangc -target spirv -profile sm_6_6+spirv_1_6` |
| DXIL sidecar (SM 6.5, d3d12) | same dxc minus `-spirv` | `slangc -target hlsl` piped through the same dxc |

The DXIL detour through HLSL is deliberate: distro slangc builds (nixpkgs
included, `SLANG_ENABLE_DXIL=FALSE`) ship without the embedded-dxc backend,
and slang's own DXIL path is exactly this lower-to-hlsl-then-dxc flow — doing
it in the build keeps one dxc for both languages and pins the DXIL feature
set to the same SM 6.5 / vkd3d envelope as the hlsl shaders.

`-matrix-layout-column-major` pins slang to dxc's default so both languages
agree with the CPU-side constant layout.

Two ergonomic differences from the hlsl flow:

- **Automatic include tracking.** slangc writes a depfile, so `#include` /
  `import` edits rebuild dependents without the manual
  `RX_SHADER_DEPS_<symbol>` bookkeeping hlsl wrapper shaders need (53 such
  lines in engine/render alone).
- **slangc is optional until used.** `RX_SLANGC` is probed at configure;
  the build only fails if a `.slang` shader is actually registered.

## Writing slang shaders for rx

Slang is close to an HLSL superset, and the dxc attribute vocabulary
(`[[vk::binding]]`, `[[vk::location]]`, `[[vk::push_constant]]`,
`: register(tN, spaceM)`) carries over unchanged. rx-specific patterns:

```slang
// Combined image sampler: one declaration for both backends. Replaces the
// hlsl double declaration ([[vk::combinedImageSampler]] Texture2D +
// SamplerState sharing a binding). Splits into tN/sN automatically on the
// hlsl->dxil path.
[[vk::binding(0, 0)]] Sampler2D src;

// Push constants: real push block on spirv, b999 cbuffer for the d3d12
// backend's root constants. Replaces rhi_bindings.hlsli's PUSH_CONSTANTS
// macro — slang never defines __spirv__, so that macro must not be used
// from slang; the dual annotation below needs no preprocessor at all.
struct Push { uint count; };
[[vk::push_constant]] ConstantBuffer<Push> push : register(b999, space0);

// Entry points carry their stage. The build still passes -stage (derived
// from the file suffix), so this is documentation more than requirement,
// but it keeps files self-describing and editor tooling working.
[shader("fragment")]
float4 main(PsIn input) : SV_Target0 { ... }
```

Buffer-device-address reads use slang pointers (`Ptr<T>`) instead of dxc's
`vk::RawBufferLoad`. Like RawBufferLoad they are SPIR-V-only
(PhysicalStorageBuffer has no DXIL mapping), so such shaders stay on the
`RX_SHADER_NO_DXIL` list either way.

## Migration status and findings

Migrated so far (each doubling as the acceptance test for a piece of the
toolchain):

- `test/shaders/offscreen_tri.{vs,ps}.slang` — spirv + dxil sidecar, covered
  by offscreen_test on vulkan and d3d12.
- `engine/render/shaders/util/fullscreen.vs.slang` — shared by every
  fullscreen pass (tonemap, wboit composite, vgeo resolve, ui blur, blits).
- `engine/render/shaders/util/blit.ps.slang` — the d3d12 BlitMip lowering;
  first user of the combined `Sampler2D` form.

Validated on NVIDIA (vulkan, validation layers clean) and vkd3d (d3d12).

The remaining inventory is 185 `.hlsl` + 25 `.hlsli` first-party files. What
a full migration would buy, by construct:

| pattern | files | slang effect |
|---|---|---|
| `[[vk::combinedImageSampler]]` pairs | 72 | one `Sampler2D` per texture instead of two annotated declarations |
| `PUSH_CONSTANTS` macro | 130 | dual annotation, macro and its `__spirv__` branch retire |
| `vk::RawBufferLoad` byte offsets | 8 | typed pointers; still NO_DXIL |
| wrapper-shader variants (`#define` + `#include` around a shared body) | ~20 | `import` + interfaces/generics; depfiles retire the manual dep lists either way |
| `#ifdef __spirv__` target splits | 6 | slang target capabilities, or often unnecessary once the constructs above are native |

Not worth migrating: shaders including vendored HLSL (NRD, FidelityFX — 6
files). They exist to match third-party headers that are and will stay HLSL;
slang can `#include` most of it, but there is nothing to win.

Recommended path: no bulk rewrite. New shaders default to slang; existing
ones migrate opportunistically when touched, leaf shaders first (post,
util, screenspace) since shared `.hlsli` headers must either stay
`#include`-compatible with both languages or be ported to slang modules as
their last hlsl includer converts. The two languages coexist indefinitely at
zero build-system cost.

One layout caution for migrated shaders: slang lays push-constant blocks out
std430 where dxc uses its cbuffer-derived rules. Structs of 16-byte-aligned
members (the norm in this codebase) are identical under both; anything with
float3/scalar mixes should be checked against the CPU struct when it moves.
