# rx: render experience

A standalone real-time rendering engine, extracted from the
[recreation](https://github.com/Force67/recreation) project. rx is the part of
that engine with nothing to do with Bethesda games: the renderer, the asset
pipeline, ECS, physics, animation and audio, plus a small viewer runtime in
place of the game.

| <img width="1440" alt="material palette" src="docs/images/material-palette.png" /> |
|:---:|
| **The material palette**: 30 named pbr presets, one cell each |

## Documentation

* [Authoring scenes](docs/AUTHORING.md): the `.rxscene` text format, `--dump-schema`, `--validate`, `--shot`
* [Character rendering](docs/CHARACTER_RENDERING.md)
* [Hair](docs/HAIR.md)
* [Slang in the shader pipeline](docs/SLANG.md), [OpenUSD loading](docs/USD.md)

## Quick start (Linux)

```sh
git submodule update --init                            # third_party/equilibrium
git clone https://github.com/Force67/kinema ../kinema  # animation runtime (sibling)
tools/get_jolt.sh                                      # physics (optional but recommended)
tools/get_fidelityfx.sh; tools/get_nrd.sh; tools/get_dlss.sh  # optional
cmake --preset linux
cmake --build build/linux
build/linux/runtime/rx --demo cornell
```

Other entry points: `rx --gltf <scene>`, `rx --usd <stage>`,
`rx --scene <file.rxscene>` and `--demo <id>` (41 of them, from cornell and
strands to the shooter gallery and the character lookdev bench). The editor
builds with `-DRX_BUILD_EDITOR=ON`.

## Requirements

* CMake 3.24+, a C++23 compiler, dxc and slangc, SDL3.
* Vulkan headers, volk and VMA are pinned and fetched by CMake.
* On NixOS use the dev shell (`nix develop`), which also provides `vkrun`
  (host NVIDIA driver bridging) and `swrun` (headless lavapipe software path).
