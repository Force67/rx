# rxc

rx behind a C ABI, for games written in languages that speak C: `librxc.so`
plus [rx_c.h](rx_c.h). The game owns its simulation and hands rx four
callbacks (init, update, build_view, shutdown); rx owns the window, the GPU,
audio and the frame loop.

- Content goes in by value (vertices, rgba8 pixels, float samples) and comes
  back as small integer handles. One material per mesh.
- Per frame: camera, draws (with a key for motion vectors), point and spot
  lights, particles, world text, debug lines, and an imgui-drawn hud.
- Raw keyboard and mouse state, relative mouse capture.
- Headless offscreen runs render too, so `rxc_screenshot` works without a
  display.

Build it with `-DRX_BUILD_RXC=ON` and the `rxc` target. Every rx module links
in statically (the option turns PIC on), and only the `rxc_*` symbols are
exported, so a host never binds rx internals. The host must load the same C
library as librxc: link it inside the dev shell. At run time the engine's
loose resources go beside the executable as `rxe/` (a copy of
`rxe/resources`), or packed as `Data/rx_engine.rxp`.

The first consumer is DEAD AIR '89, a Quake-style shooter written in aki.
