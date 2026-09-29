# rx

A real-time rendering engine. C++23, Vulkan and D3D12 behind an RHI, HLSL/Slang
shaders. Code lives in four folders, each depending only on the ones before it:
`foundation/` (what rx adds to equilibrium base), `rxe/` (the engine modules),
`plugins/` (optional modules a game enables) and `apps/` (the viewer in
`apps/shell`, the editor, the tools). [docs/STRUCTURE.md](docs/STRUCTURE.md) has
the rules; `tools/checkincludes` enforces them as a ctest.

## Building and running

NixOS: everything goes through the dev shell.

```sh
nix develop -c cmake --build build/linux
cd build/linux && nix develop <repo> -c ctest
```

For day-to-day work `nix develop -c cmake --preset linux-dev` (then build
`build/linux-dev`) builds every module as its own `.so`, so an edit relinks one
library. Benchmarks and captures you compare stay on `build/linux`.

The binary needs the dev shell at runtime too (`libvkd3d`), so prefix runs with
`nix develop -c`, including read-only ones like `--validate` and `--dump-schema`.

Anything that renders must be wrapped in `vkrun` (real GPU) or `swrun`
(software). **A run with neither has no Vulkan loader, silently falls back to a
stub that writes no png, and exits nonzero.** Under `swrun` pass `--no-rt` or
`--preset low`; lavapipe's acceleration-structure builds crash independently of
anything you change.

## Authoring content

Scenes are text (`.rxscene`) and are meant to be written directly. Read
[docs/AUTHORING.md](docs/AUTHORING.md) before writing one. In short:
`rx --dump-schema` and `rx --dump-materials` are generated from reflection and
list everything available; `rx --validate` checks a scene without a GPU;
`--shot` renders headless and exits nonzero if no png appeared.

## Verifying a render change

Captures are deterministic (`--shot` implies a lockstep clock), so a change that
should not move the picture can be **proven** not to:

```sh
./build/linux/apps/rxdiff/rxdiff before.png after.png     # rmse limit 0.002, measured floor 0.00055
```

- Diff at **20+ frames**. At 8, a busy scene's own noise can exceed the limit.
- Never compare by hash: deterministic is not bit-identical.
- The shipped scenes in `apps/shell/scenes/` are the reliable numeric surface. The
  feature gym tour is **not**; several of its stops flip bimodally between
  process launches, and `testing/feature_gym/tour.py` cannot run here at all.
- **GPU-backed tests skip with exit 0 with no Vulkan loader**, and plain `ctest`
  has none, so they pass vacuously. Run them under `vkrun` directly to know they
  executed.

## Conventions

- Namespace `rx::`; env knobs are `RX_*`. Grep for `base::Option`.
- The C++ standard library is banned, and so are exceptions (rx builds with
  `-fno-exceptions`). Use `base::` (third_party/equilibrium/base) and the rx
  helpers in `foundation/`: `math/scalar.h` (`rx::Min/Max/Clamp`, std
  semantics; never `base::Min/Max/Clamp`, which differ on NaN),
  `strings/format.h` (`rx::StrFormat`/`ToString`, exact `std::format`/
  `to_string` text), `files/file_system.h` (`rx::fs`),
  `strings/text_reader.h`/`text_writer.h` (getline, `>>`, ostream),
  `algorithm/sort.h` (`rx::StableSort`, `rx::NthElement`), `memory/shared.h`. Allowed
  std: `std::initializer_list`, placement new, `std::align_val_t`/`nothrow_t`
  in operator new/delete, and third-party signatures that demand std types
  (tinyusdz in `usd_loader.cc`, libultragui's `ugui::String`).
- `base::Sort` is not `std::sort`: replace a sort only where keys are unique,
  or use `rx::StableSort`.
- A check that must stop the process is `BASE_FATAL_CHECK`. `BASE_BUGCHECK` and
  `BASE_DCHECK` only break into a debugger and continue.
- Comments explain **why**, and document invariants and failure modes. They do
  not restate the line.
- Prefer failing a load loudly with a `path:line:` message over substituting a
  default. Silent correction is the failure mode this format works hardest to
  avoid.
