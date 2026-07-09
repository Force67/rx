# Plan: geometry providers & renderer extraction

**Status: PLAN ONLY — not implemented, not scheduled.** Written 2026-07-08 after
assessing whether this renderer could host foreign rendering paradigms
(specifically the voxel/SDF paths in `~/Documents/Projects/anyvoxel`) and
eventually stand alone as its own library. Conclusion: yes, incrementally.
This file records the design so the work can start from a decision, not a
re-derivation. See README.md for the current architecture; nothing in here
changes it yet.

## Motivation

The layering is already right: `rhi/` + render graph + the pass library are
paradigm-agnostic, and everything Skyrim-mesh-shaped is quarantined in the
`Renderer`/`BuildFrameGraph` composition layer. A voxel or SDF world is a
*sibling composition* on the same substrate — not a bolt-on to `Renderer`.
What's missing is a name and a written contract for that seam, plus a handful
of RHI features streaming worlds need. Precedent: kinema (libs/kinema), which
went from engine-embedded animation code to a reusable library with a thin
adapter, the engine becoming its first consumer.

## Core concept: GeometryProvider

A provider is anything that, given the frame context (matrices, jitter,
target handles), adds render-graph passes fulfilling up to three contracts:

1. **G-buffer contract** (required) — write depth, oct-normal+roughness,
   motion vectors and depth-export at render resolution with the frame's
   jitter. This is the load-bearing seam: everything downstream (DDGI,
   ReSTIR, SSAO/SSR/SSGI, froxel fog, TAA, upscalers, the post chain)
   consumes these targets and does not care who wrote them. The formats are
   the `k*Format` constants in `core/renderer.h`; making a provider means
   promoting them from folklore to interface.
2. **Shadow-caster contract** (optional) — record depth-only draws into the
   per-cascade/per-face callbacks the CSM and local-shadow passes already
   use. The existing callback shape is nearly the interface already.
3. **TLAS contribution contract** (optional) — contribute BLAS instances so
   visibility rays (RT shadows, RTAO, contact shadows) work over any
   geometry mix.

The current mesh world (mesh pipeline, GPU cull, meshlets, material system)
becomes `MeshWorldProvider`, the first implementation. Voxel quad-arena and
SDF surface-nets worlds become the second and third, inheriting sky, DDGI,
froxel fog, RT shadows, TAA/FSR/DLSS, DRS, HDR, the profiler and the golden
harness on day one.

**Known hard part:** full *radiance* ray tracing (path tracer, DDGI ray
shading) needs hit shading, and the bindless hit tables are mesh-record
shaped (`instanceCustomIndex -> mesh -> geometry -> material`).
Visibility-only rays need nothing. A voxel provider can register its quad
arena as mesh records with procedural materials, but path-trace parity for
foreign providers is explicitly out of scope for the first phases.

## Phases (each independently shippable)

- **Phase 0 — enablers.** (Unconditionally useful even if providers never
  ship.)
  - Upload ring: non-blocking streaming uploads (also fixes the texture
    streamer's blocking promotes).
  - `DrawIndirectCount` / non-indexed `DrawIndirect` / `DispatchIndirect`
    in the RHI (both backends + null). Today GPU culling zeroes instance
    counts; streaming worlds want GPU-written draw counts.
  - Promote the deferred-destroy retire ring (currently private to
    MaterialSystem) into the Device (`DestroyDeferred`); every streaming
    use case needs it (BLAS churn, chunk buffers, set swaps).
- **Phase 1 — contract extraction.** Define `FrameContext` + the provider
  interface; move the mesh-world code behind `MeshWorldProvider`; document
  the three contracts in README.md. Mechanical but large; pixel-identical by
  construction — goldens are the proof.
- **Phase 2 — proof of concept.** Minimal SDF provider (surface-nets
  terrain; anyvoxel's implementation is ~150 lines) writing the G-buffer;
  `--demo sdf` renders procedural terrain lit by this renderer's sun/sky/
  DDGI with TAA. One small demo validates the whole architecture before any
  large refactor is trusted.
- **Phase 3 — voxel world.** Quad arena, GPU cull with count-buffer draws,
  chunk streaming through the upload ring, optional mesh-shader
  amplification reusing the meshlet infra. Effectively anyvoxel's renderer
  rehosted; its Slang shaders port to HLSL (BDA reads join the
  `RX_SHADER_NO_DXIL` list, same as the meshlet path).
- **Phase 4 — become its own thing.** Extraction a la kinema:
  `engine/render` -> standalone library. The real coupling to break is the
  upload API speaking `asset::` types (`UploadMesh(asset::Mesh)`,
  `UploadTexture(asset::Texture)`, `asset::Material`) — the library needs
  renderer-owned upload descriptions with a thin adapter on the rx
  side. Window/math/log resolve to equilibrium BASE, which anyvoxel already
  uses — the same foundation under both consumers. Slang could join as an
  optional shader front-end here (emits SPIR-V + DXIL, fits the dual-target
  embed pipeline).

## Deliberate non-goals

- No generic scene graph. Providers own their world representations (the
  ECS hands MeshWorldProvider a draw list; a voxel provider owns its chunk
  map).
- No D3D12 parity for BDA-pointer shaders — that's physics, not
  architecture. Caps-gating + `NO_DXIL` is the answer; anyvoxel would adopt
  it rather than fix its blind DX12 backend.
- No merging of the two projects' ECS/game layers. This is about the
  renderer only.
