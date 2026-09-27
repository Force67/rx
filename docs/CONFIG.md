# Content, mounts and platform config

How an rx executable finds its files, and how the engine and a game decide
how to run on a given machine. Everything goes through the vfs
(`engine/asset/vfs.h`); nothing is compiled into the binary.

## The install layout

An rx game is one executable with the engine linked in. Its content sits in
the directory beside it:

```
<install>/
  <game>                  the executable
  rxe/config/*.ini        -> rxe://config/       the engine's platform tiers
  config/*.ini            -> <title>://config/   the game's platform config
  Data/rx_fonts.rxp       -> rxe://fonts/        the engine's archives, by name
  Data/*.rxp              -> <title>://          every other archive
  ...                     -> <title>://          any other loose file of the game
```

`asset::MountContent` (called by `app::Host` at startup) mounts it:

| mount | what | from |
|---|---|---|
| `rxe://` | everything the engine owns | `Data/` archives it knows by name (`rx_fonts.rxp` -> `rxe://fonts/`), then `rxe/` loose |
| `<title>://` | everything the game owns | the other `Data/*.rxp` in name order, then the install directory loose |

`<title>` is `AppConfig::title` (`anyvoxel` -> `anyvoxel://`). Archives mount
before loose files and later mounts win, so a loose file overrides the packed
one and `02_patch.rxp` overrides `01_base.rxp`. An application mounts anything
else it needs after `OnInitialize` starts, over all of this.

| env | effect |
|---|---|
| `RX_CONTENT_DIR` | use this directory instead of the executable's |
| `RX_ENGINE_ARCHIVES` | take the engine's archives from here instead of `Data/` |

### In a build tree

The executable runs from the build directory, so CMake reproduces the layout
beside it on every build (re-copied, so an edited ini needs no relink):

```cmake
rx_stage_content(mygame mytool)   # rxe/config + Data/ beside both (same directory)
add_custom_target(mygame_config ALL COMMAND ${CMAKE_COMMAND} -E
  copy_directory_if_different ${CMAKE_CURRENT_SOURCE_DIR}/config $<TARGET_FILE_DIR:mygame>/config)
add_dependencies(mygame mygame_config)
```

Executables that share a directory go in one `rx_stage_content` call, or two
copies race into the same files. The source of each side is the project's
`config/` folder: `rx/config/` for the engine, `<game>/config/` for a game.

## Platform config

A platform config is a set of ini files deciding how the engine and the game
run on one kind of machine. Each section belongs to the subsystem that owns
its keys:

| section | owner | keys |
|---|---|---|
| `[render]`, `[render.<group>]` | `RenderSettings` | the `render::ApplyIni` keys (`SettingsToIni` lists them all); `<group>` is for reading only |
| `[memory.arena]`, `[memory.pools]`, `[memory.budgets]` | the memory plan | `frame_mb`, `ecs_chunks`, `<category> = MiB` |
| `[options]` | any registered `base::Option` | its name: `win.fullscreen = true`, `unfocused.fps = 5` |

```ini
# mygame/config/steamdeck.ini
include = rxe://config/low.ini   # start from a different engine tier

[render.antialiasing]
sharpness = 0.5

[memory.budgets]
assets = 768

[options]
unfocused.fps = 5
```

`include = <vfs path>` applies that file first, so the including file's keys
win wherever it names them. Includes nest up to 8 deep.

Every line has to land somewhere: an unknown section, an unknown render key, a
value that does not parse, an option nobody registered or an include that does
not resolve is logged as `path:line: ...` and counted. The host reports the
count (`platform config with N problem(s)`).

### Which files, in what order

The quality tier (`render::QualityPreset`) is known once the gpu is up:
`DetectPreset` recognizes a Steam Deck by its board and mobile gpus by name,
and places desktops by vram and ray query. A tier's config is read in this
order, later winning:

1. `rxe://config/default.ini` (optional)
2. `rxe://config/<tier>.ini` (required: startup stops without it)
3. `<title>://config/default.ini` (optional)
4. `<title>://config/<tier>.ini` (optional)
5. the file `RX_CONFIG` names, from disk or a vfs path (optional), for tuning
   on a device without a rebuild

A game only writes the tiers it has an opinion on, and only the keys it
changes. Tier names are the `PresetName`s: `android_low`, `android_medium`,
`android_high`, `steamdeck`, `low`, `console`, `medium`, `high`, `ultra`.

Before the gpu is up only the `default.ini` files (and `RX_CONFIG`) are read,
and their `[options]` and memory plan apply then. So options consumed at
startup (window size, fullscreen, touch-as-mouse) belong in `default.ini`; a
tier file's options and memory plan apply once the tier is known.

### Precedence

For render settings: built-in defaults < the files above in order < `RX_*`
env overrides that are actually set < `AppConfig::tune_settings`. The device
clamps come after the files: a tier cannot turn on ray tracing a gpu lacks.
For options: built-in default < the files < the environment.

### The engine's tiers

`rx/config/` holds one file per tier. `steamdeck.ini` is spelled out in full;
the others include their neighbour and list what they change, each with the
hardware it targets and how its numbers were measured or estimated:

```
steamdeck ─┬─ android_high ─ android_medium ─ android_low
           └─ low ─ medium ─┬─ console
                            └─ high ─ ultra
```

The rx viewer's debug ui (Renderer panel) edits these source files: Load
applies a file's render keys to the live settings, Save writes the current
ones as `[render.*]` sections. Saving over a tier drops its includes and its
other sections, so save under a new name and merge by hand.
