# engine/assets

Content rx itself ships. Each subdirectory is packed into its own `.rxp`
archive at build time (see the `rx_engine_archives` target) and mounted by
`asset::MountEngineArchives` under the namespace it is named for, ahead of any
game archive so a game can override it:

| directory | archive        | mount point |
| --------- | -------------- | ----------- |
| `fonts/`  | `rx_fonts.rxp` | `fonts://`  |

## fonts

Roboto 3.016, unhinted static instances from
[googlefonts/roboto-3-classic](https://github.com/googlefonts/roboto-3-classic),
under the SIL Open Font License 1.1 (`roboto/OFL.txt`). Regular is the default
imgui font (`render::kRxDefaultFontPath`); Medium and Bold are there for UI that
wants weight.
