# Memory pools investigation (2026-07-14)

Question: should rx add memory pools, or is mimalloc enough? Where would pools fit the
ECS design, how do we track per-category consumption (equilibrium `memory_stat_tracker`),
and how do we visualize it — with Steam Deck / mobile predictability as the driver.

## Verdict

**Targeted pools + arenas: yes. Blanket "pool everything": no.** mimalloc already wins the
general-purpose speed race (~single-digit ns per small alloc); hand-rolled generic pools
won't beat it on throughput. What mimalloc *cannot* give us — and what pools/arenas do —
is:

1. **Contiguity for iteration** (ECS chunked columns → cache-friendly, no realloc copies)
2. **Bounded, pre-reserved footprint** (no long-session fragmentation growth on
   Deck/mobile; peak = configured, not emergent)
3. **Bulk lifetime** (frame arena: one pointer reset instead of N frees)
4. **Exact per-subsystem accounting** (a pool knows its size; a heap does not)

Also note: **`RX_MIMALLOC` is OFF by default** (`CMakeLists.txt:22`) — default builds run
plain glibc malloc today. Turning it on is step zero before any perf comparison.

## Findings

### ECS (engine/ecs) — structural problems, pools are the right fix

Archetype/columnar SoA, components as raw bytes in `base::Vector<u8>` columns
(`archetype.h:73-77`). Not chunked. Three real defects:

- **O(n²) archetype fill.** `Archetype::AddRow` (`archetype.cc:23-30`) grows each column
  with `resize(size + stride)`, and `base::Vector::resize` grows capacity to *exactly* n
  (equilibrium `vector.h:176-192`) — every single spawned entity reallocates and copies
  every column. Realloc storm + per-spawn latency spikes + fragmentation driver.
- **Non-POD relocation gap.** Row-level ops (swap-remove, archetype transition) correctly
  use `ComponentInfo::move_construct/destruct` thunks, but column *growth* relocates via
  the byte-vector's memcpy path, bypassing the thunks. Safe only for trivially relocatable
  components; a future `std::string`-holding or self-referential component corrupts
  silently.
- **Churn temporaries.** One heap alloc (signature vector) per `World::Each` call per
  system per frame (`world.h:48-58`); two per archetype transition (`world.cc:104-114`).
  Column memory is a high-water mark that never returns (`SwapRemoveRow` shrinks size,
  not capacity); empty archetypes are never reclaimed.

**Fix that fits the design:** chunked column storage — fixed-size chunks (16 KiB, the
flecs/Unity convention) drawn from one central **ChunkPool** (single fixed block size →
zero external fragmentation, O(1) amortized row append, freed chunks return to the pool
on entity destruction). Grow never relocates existing rows, so the non-POD gap closes for
free (and cross-chunk moves go through `move_construct`). `AddRow`/`SwapRemoveRow`/
`Column` is a clean single choke point; `World::Each` iteration becomes per-chunk pointer
walks. Side benefits: per-archetype memory accounting is trivial, and stable row addresses
within a chunk.

Interim one-liner (before chunking lands): make `AddRow` grow amortized (reserve 2×) —
kills the O(n²) today.

### Engine-wide hot spots (ranked, main offenders)

1. `FrameView` rebuilt from empty every frame + fresh unreserved
   `base::UnorderedMap<u64, Mat4>` per frame in `GatherEntityDraws`
   (`app/host.cc:226,249`) — worst per-frame churn, scales with entity count.
2. Render-graph pass closures are `std::function`, **63 AddPass/frame**
   (`render_graph.h:104`, `renderer.cc:1459-3752`); over-SBO captures = 60–120 heap
   allocs/frame independent of scene.
3. `WriteDescriptors` allocates 5 vectors per call, ~85 `BindTransient` sites → ~250+
   tiny allocs/frame (`vk_device.cc:1348-1352`); items ≤8 → wants small_vector/stack.
4. RenderGraph `Stats` snapshot rebuilt (with string copies) every frame even with the
   inspector closed (`render_graph.cc:183-208`).
5. Transient gather vectors in BuildFrameGraph (transparent list, RT instances, particle
   buffers) — fresh per frame (`renderer.cc:1522,1960,3281`).
6. **Audio mixer allocates on the realtime SDL callback thread** (`mixer.cc:87,150`) —
   per-voice decode chunk vector per callback; underrun risk, pure predictability bug.
7. Texture streaming creates/destroys a VMA staging buffer per streamed texture
   (`material_system.cc:164-222`) — GPU-heap fragmentation on Deck/mobile.
8. Load-time spikes: mesh bake / glTF decode / physics cook / RPC encode transient
   vectors (`meshlet.cc`, `gltf_loader.cc`, `physics_world.cc:242`, `rpc_message.cc:90`).

Existing good patterns to extend: imgui renderer's grow-only persistent-mapped buffer
ring, RenderGraph TransientPool for GPU images, per-frame transient descriptor pool, job
system's inline `StaticFunction<void(),256>`, Jolt's preallocated 16 MB TempAllocator.

### Threading → pool domains

Three concurrent allocation domains; keep them separate (no shared locks):
(a) main/render thread — frame arena covers hotspots 1–5;
(b) audio callback thread — preallocated per-voice buffers, nothing shared;
(c) streaming workers (VT + texture upload) — staging ring + reusable page scratch.

## Proposed architecture

```
engine/core/memory/
  memory.h            category tokens + MemoryCategoryScope + counters (port of
                      equilibrium base/allocator/memory_stat_tracker.{h,cc})
  frame_arena.h       linear/bump allocator, reset at frame start; STL-style adapter
  chunk_pool.h        fixed 16KiB blocks, freelist, pre-reserve from config
  small_vector.h      inline-storage vector (descriptor scratch, signatures)
  memory_config.{h,cc} budgets/reservations table (see below)
```

- **Tracking over mimalloc** (equilibrium pattern): define global `operator new/delete`
  in rx (when `RX_MIMALLOC` on) that call `mi_malloc`/`mi_free` and
  `TrackOperation(ptr, ±size)` using `mi_usable_size` for exact decrement on free —
  exactly equilibrium's `MimallocRouter` contract. Thread-local `MemoryCategoryScope`
  at subsystem entry points (asset load, ECS structural ops, render frame build, audio,
  net, physics). Raw C `malloc` from third parties stays in `<general>` but is still
  counted via mimalloc interposition + `mi_stats`. Pools report their own exact numbers
  and *also* charge their backing allocations to their category.
- **Pool config**: the "XML with initial expected sizes" ancestor is Gamebryo's memory
  pool config / CryEngine's memory manager tables. Modern shape: a small declarative
  table (compile-time defaults + `memory.ini` override next to `controls.ini`) per named
  pool/category: `initial_reserve`, `soft_budget`, `hard_max(0=unbounded)`. Platform
  presets (desktop / steamdeck / mobile) pick reservation sizes. Budgets are *soft*
  (warn in HUD) except for pools that are structurally bounded (frame arena size,
  staging ring size).
- **imgui panel**: new "Memory" section in `runtime/debug_ui.cc` `DrawDiagnosticsTab`,
  mirroring the existing "GPU memory" header (`debug_ui.cc:552-570`): per-category table
  (name, current, peak, budget, progress bar, red over budget), per-pool rows (chunk
  pool used/free chunks, frame-arena high-water vs capacity, staging-ring occupancy),
  plus the existing VMA `Device::memory_budget()` and per-frame alloc-count delta
  (mimalloc stats) to catch regressions.

## Priority order

0. `RX_MIMALLOC=ON` by default (keep sanitizer exclusion).
1. `AddRow` amortized growth fix (one-liner; immediate O(n²) kill).
2. Frame arena + convert render-graph pass closures to inline-storage functions +
   persist/reserve `FrameView` + transform map (hotspots 1,2,5) + small_vector for
   `WriteDescriptors` (3) + gate Stats behind inspector (4).
3. Category tracker port + operator new hook + scopes; imgui Memory panel (visibility
   before the big ECS change, so the win is measurable).
4. ECS chunked columns on ChunkPool with typed relocation (closes non-POD gap).
5. memory.ini budgets + platform presets.
6. Audio-thread preallocation; texture-upload staging ring.

## Non-goals

- Per-type object pools scattered across gameplay code — mimalloc already bins by size;
  the maintenance cost isn't paid back.
- Replacing mimalloc with a custom general heap.
- Per-category `mi_heap_t` isolation — possible later (heap-per-thread-per-category),
  but scope-token accounting gives 90% of the value at 10% of the complexity.
