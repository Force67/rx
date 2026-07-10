rx_add_module(audio
  audio_clip.cc
  wav.cc
  xwma.cc
  mixer.cc
  audio_device.cc
  audio_system.cc
)

# Sound bytes come from the application's Vfs; the mixer math and decoders only
# need core + asset. The SDL output backend is optional: without it (the Android
# native backend, a headless server) audio_device.cc compiles to a silent stub
# and the rest of the module is unchanged.
target_link_libraries(rx_audio PUBLIC
  rx::core rx::asset)

if(SDL3_FOUND)
  target_link_libraries(rx_audio PUBLIC SDL3::SDL3)
  target_compile_definitions(rx_audio PRIVATE RX_HAS_SDL3_AUDIO=1)
endif()

# The codec backend for compressed game audio (xWMA/FUZ/Wwise). Exactly one of
# the two implementations is compiled: the real FFmpeg one when the option is on
# and libav* is found, otherwise a stub that skips those formats with a note.
if(RX_AUDIO_FFMPEG)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec libavformat libavutil libswresample)
  target_sources(rx_audio PRIVATE ffmpeg_codec.cc)
  target_link_libraries(rx_audio PUBLIC PkgConfig::FFMPEG)
  target_compile_definitions(rx_audio PRIVATE RX_AUDIO_FFMPEG=1)
  message(STATUS "audio: FFmpeg backend enabled (xWMA/Wwise/FUZ)")
else()
  target_sources(rx_audio PRIVATE ffmpeg_stub.cc)
endif()
