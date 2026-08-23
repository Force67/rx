# rx

A real-time rendering engine. C++23, Vulkan and D3D12 behind an RHI, HLSL/Slang
shaders. See `README.md` for what each module does.

## Building and running

NixOS: everything goes through the dev shell.

```sh
nix develop -c cmake --build build/linux
cd build/linux && nix develop <repo> -c ctest
```

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
./build/linux/rxdiff before.png after.png     # rmse limit 0.002, measured floor 0.00055
```

- Diff at **20+ frames**. At 8, a busy scene's own noise can exceed the limit.
- Never compare by hash: deterministic is not bit-identical.
- The shipped scenes in `runtime/scenes/` are the reliable numeric surface. The
  feature gym tour is **not**; several of its stops flip bimodally between
  process launches, and `tests/feature_gym/tour.py` cannot run here at all.
- **GPU-backed tests skip with exit 0 with no Vulkan loader**, and plain `ctest`
  has none, so they pass vacuously. Run them under `vkrun` directly to know they
  executed.

## Conventions

- Namespace `rx::`; env knobs are `RX_*`. Grep for `base::Option`.
- The tree mixes `base::` containers and std. Match the file you are editing;
  do not convert either direction.
- Comments explain **why**, and document invariants and failure modes. They do
  not restate the line.
- Prefer failing a load loudly with a `path:line:` message over substituting a
  default. Silent correction is the failure mode this format works hardest to
  avoid.
