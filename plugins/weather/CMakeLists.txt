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
target_link_libraries(rx_weather PUBLIC rx::foundation rx::render rx::audio)

if(RX_BUILD_TESTS)
  # rx::weather unit tests: pure CPU logic (deterministic scheduling, region
  # restriction, transition blending, scripted override, surface + wind + lightning
  # integrals), no GPU, so plain `ctest` runs them everywhere.
  add_executable(weather_test weather_test.cc)
  target_link_libraries(weather_test PRIVATE rx::weather rx::foundation)
  add_test(NAME weather_test COMMAND weather_test)

  add_executable(thunder_synth_test thunder_synth_test.cc)
  target_link_libraries(thunder_synth_test PRIVATE rx::weather rx::audio rx::foundation)
  add_test(NAME thunder_synth_test COMMAND thunder_synth_test)
endif()
