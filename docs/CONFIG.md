# Identity, content and platform config

How an rx app says who it is, how it finds its files, and how the engine, the
game and the player decide how it runs on a given machine. Every file goes
through the vfs (`rxe/asset/vfs.h`); nothing is compiled into the binary.

## Identity

Every app on `app::Host` sets three things in `AppConfig`. All are required;
startup stops on a bad one.

| field | example | rules | used for |
|---|---|---|---|
| `id` | `czs58msczjwar18br5tnv4bk` | a cuid2 (`rx --new-app-id`) | what only machines see: the cache folder, anything that must be unique across every rx app and survive a rename |
| `name` | `anyvoxel` | slug: lowercase `[a-z0-9_-]`, starting with a letter; not `rxe` or `user` | what users see: the vfs namespace `<name>://`, the settings folder, log tags |
| `title` | `anyvoxel` | any text | what people read: the window title, the application name drivers and compositors see |

rx's own tools carry one too (the viewer is `rx`, the editor `rx-editor`).
Code that needs it reads `GetAppIdentity()` (`foundation/system/app_identity.h`).

Per-user folders, created on first use:

| | Linux | Windows | macOS |
|---|---|---|---|
| settings (`UserConfigDirectory`) | `$XDG_CONFIG_HOME/<name>` (`~/.config/<name>`) | `%APPDATA%\<name>` | `~/Library/Application Support/<name>` |
| cache (`UserCacheDirectory`) | `$XDG_CACHE_HOME/<id>` (`~/.cache/<id>`) | `%LOCALAPPDATA%\<id>` | `~/Library/Caches/<id>` |

The settings folder holds what a player changes (`controls.ini`, `config/*.ini`)
under the name they recognize. The cache folder holds what can be rebuilt (the
Vulkan pipeline cache) under the id no other app shares. The texture and
.blend import caches stay shared in `~/.cache/rx/`: they are keyed by content,
so every rx app gains from them.

## The install layout

```
<install>/
  <game>                  the executable, engine linked in
  Data/rx_engine.rxp      -> rxe://           everything the engine ships: fonts/, config/
  Data/*.rxp              -> <name>://        the game's archives, in name order
  config/*.ini            -> <name>://config/ the game's platform config (packed or loose)
  ...                     -> <name>://        any other loose file of the game
  rxe/config/*.ini        -> rxe://config/    engine builds only: the tiers, loose and editable
```

`asset::MountContent` (called by `app::Host` at startup) mounts it, archives
before loose files, later mounts winning:

| mount | from, in mount order |
|---|---|
| `rxe://` | `Data/rx_engine.rxp`, then `rxe/` loose if present |
| `<name>://` | every other `Data/*.rxp` in name order (`02_patch.rxp` over `01_base.rxp`), then the install directory loose |
| `user://` | the settings folder |

A game ships `rx_engine.rxp` untouched and adds its own archives beside it, split
however it likes (a base pack, DLC, patches). An engine build (the viewer, the
editor) additionally ships the tiers loose in `rxe/config/`, which override the
packed ones file by file, so they can be edited in place. An application mounts
anything else it needs after `OnInitialize` starts, over all of this.

| env | effect |
|---|---|
| `RX_CONTENT_DIR` | use this directory instead of the executable's |
| `RX_ENGINE_ARCHIVES` | take `rx_engine.rxp` from here instead of `Data/` |

### In a build tree

CMake packs `rxe/resources/` (fonts and `config/`) into `Data/rx_engine.rxp` and
reproduces the layout beside build-tree executables, re-copied every build:

```cmake
rx_stage_content(mygame mytool)               # Data/ beside both (same directory)
rx_stage_content(rx_editor LOOSE_ENGINE_CONFIG)   # engine builds: + rxe/config/
add_custom_target(mygame_config ALL COMMAND ${CMAKE_COMMAND} -E
  copy_directory_if_different ${CMAKE_CURRENT_SOURCE_DIR}/config $<TARGET_FILE_DIR:mygame>/config)
add_dependencies(mygame mygame_config)
```

A game packs its own archives with `rx_add_archive`: CONTENT pairs name a
directory in the archive and a source directory or file, DEPENDS lists
generated files (compiled shaders) that have nothing to glob at configure time.

```cmake
rx_add_archive(mygame_archive OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/Data/mygame.rxp
  CONTENT shaders ${SHADER_OUT_DIR}/lit.spv
          terrain ${CMAKE_CURRENT_SOURCE_DIR}/assets/terrain/tile.bin
          ui      ${CMAKE_CURRENT_SOURCE_DIR}/assets/ui)
add_dependencies(mygame mygame_archive)
```

One big archive is fine: mounting reads only its table of contents, and a read
decompresses just that entry. Reads from one archive share a lock, so a game
streaming heavily from many threads may want its streamed content split out.

Executables sharing a directory go in one `rx_stage_content` call, or two copies
race into the same files. Staging copies but never deletes: a renamed or removed
file lingers in a build tree until the directory is cleaned.

## Platform config

A platform config is a set of ini files deciding how the engine and the game
run on one kind of machine. Each section belongs to the subsystem that owns its
keys:

| section | owner | keys |
|---|---|---|
| `[render]`, `[render.<group>]` | `RenderSettings` | the `render::ApplyIni` keys (`SettingsToIni` lists them all); `<group>` is for reading only |
| `[memory.arena]`, `[memory.pools]`, `[memory.budgets]` | the memory plan | `frame_mb`, `ecs_chunks`, `<category> = MiB` |
| `[options]` | any registered `base::Option` | its name: `win.fullscreen = true`, `unfocused.fps = 5` |

```ini
# anyvoxel://config/steamdeck.ini
include = rxe://config/low.ini   # start from a different engine tier

[render.antialiasing]
sharpness = 0.5

[memory.budgets]
assets = 768
```

`include = <vfs path>` applies that file first, so the including file's keys
win wherever it names them. Includes nest up to 8 deep.

Every line has to land somewhere: an unknown section, an unknown render key, a
value that does not parse, an option nobody registered or an include that does
not resolve is logged as `path:line: ...` and counted; the host reports the
count (`platform config with N problem(s)`).

### Layers, in order

The quality tier (`render::QualityPreset`) is known once the gpu is up:
`DetectPreset` recognizes a Steam Deck by its board and mobile gpus by name, and
places desktops by vram and ray query. A tier's config is read in this order,
each layer overriding the one before key by key:

| layer | files | ships in |
|---|---|---|
| engine | `rxe://config/default.ini`, `rxe://config/<tier>.ini` (required) | `rx_engine.rxp` |
| game | `<name>://config/default.ini`, `<name>://config/<tier>.ini` | the game's `config/` or archive |
| player | `user://config/default.ini`, `user://config/<tier>.ini` | the settings folder |
| tuning | the file `RX_CONFIG` names, disk or vfs path | wherever you point it |

Everything but the engine's tier file is optional. A game writes only the tiers
it has an opinion on, and only the keys it changes; a player (or a settings
menu) does the same in the settings folder. Changing a tier by overriding keys
keeps the rest of it following engine updates; replacing the engine's file
(a loose `rxe/config/<tier>.ini`) takes over the whole tier.

Tier names are the `PresetName`s: `android_low`, `android_medium`,
`android_high`, `steamdeck`, `low`, `console`, `medium`, `high`, `ultra`.

Before the gpu is up only the `default.ini` files (and `RX_CONFIG`) are read,
and their `[options]` and memory plan apply then. So options consumed at
startup (window size, fullscreen, touch-as-mouse) belong in `default.ini`; a
tier file's options and memory plan apply once the tier is known.

### Precedence

For render settings: built-in defaults < the layers above in order < `RX_*`
env overrides that are actually set < `AppConfig::tune_settings`. The device
clamps come after the files: a tier cannot turn on ray tracing a gpu lacks.
For options: built-in default < the layers < the environment.

### The engine's tiers

`rxe/resources/config/` holds one file per tier. `steamdeck.ini` is spelled out in full;
the others include their neighbour and list what they change, each with the
hardware it targets and how its numbers were measured or estimated:

```
steamdeck ─┬─ android_high ─ android_medium ─ android_low
           └─ low ─ medium ─┬─ console
                            └─ high ─ ultra
```

The rx viewer's debug ui (Renderer panel) edits these source files: Load
applies a file's render keys to the live settings, Save writes the current ones
as `[render.*]` sections. Saving over a tier drops its includes and its other
sections, so save under a new name and merge by hand.
