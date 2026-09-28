# rx::weather: the art-directable weather-STATE layer for the Cloudscape
# volumetric cloud system. Decides WHAT atmospheric
# conditions exist and where (states, regions, a deterministic scheduler,
# transitions and the wind/surface/lightning integrals) and emits the two
# renderer value structs each frame. It never touches the GPU, but those structs
# are render's own types (render/core/settings.h pulls in the render graph and
# RHI headers), so it links rx::render rather than borrowing its headers.
#
# thunder_synth renders the thunder a strike schedules into an audio clip.
rx_add_module(weather
  weather.cc
  thunder_synth.cc
)
target_link_libraries(rx_weather PUBLIC rx::core rx::render rx::audio)
