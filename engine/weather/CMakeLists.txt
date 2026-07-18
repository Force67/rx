# rx::weather -- the art-directable weather-STATE layer for the Cloudscape
# volumetric cloud system (docs/WEATHER.md). Decides WHAT atmospheric
# conditions exist and where (states, regions, a deterministic scheduler,
# transitions and the wind/surface/lightning integrals) and emits the two
# renderer value structs each frame. Headless and GPU-free: it only reads the
# render module's public control/settings headers and its lightning envelope,
# so it links rx::render but pulls in no pass or device code of its own.
rx_add_module(weather
  weather.cc
)
target_link_libraries(rx_weather PUBLIC rx::core rx::render)
