# engine/assets

Content rx itself ships. It is packed with the platform tiers from `config/`
into one archive, `Data/rx_engine.rxp`, at build time (the `rx_engine_archives`
target) and mounted at `rxe://` by `asset::MountContent`
([docs/CONFIG.md](../../docs/CONFIG.md)):

| directory       | mount point      |
| --------------- | ---------------- |
| `fonts/`        | `rxe://fonts/`   |
| `../../config/` | `rxe://config/`  |

## fonts

Roboto 3.016, unhinted static instances from
[googlefonts/roboto-3-classic](https://github.com/googlefonts/roboto-3-classic),
under the SIL Open Font License 1.1 (`roboto/OFL.txt`). Regular is the default
imgui font (`render::kRxDefaultFontPath`); Medium and Bold are there for UI that
wants weight.
