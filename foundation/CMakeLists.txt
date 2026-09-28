# rx::foundation: what rx adds to equilibrium base (docs/STRUCTURE.md). One
# module, like Chromium's base, with a subdirectory per topic. Nothing here may
# include rxe/ or plugins/.
rx_add_module(foundation
  logging/log.cc
  strings/format.cc
  strings/text_reader.cc
  files/file_system.cc
  files/paths.cc
  tasks/job_system.cc
  features/feature_registry.cc
  time/frame_timer.cc
  system/platform.cc
  system/app_identity.cc
  memory/memory_tracker.cc
  memory/frame_arena.cc
  memory/chunk_pool.cc
  memory/memory_config.cc
)

# equilibrium's base library; linking it here makes base:: containers and
# strings available to every engine module.
# equilibrium::base is wrapped in BUILD_INTERFACE so it is not written into the
# installed export set (find_package consumers get it back as a bundled imported
# target from rxConfig.cmake); in-tree consumers still link it normally.
target_link_libraries(rx_foundation PUBLIC
  $<BUILD_INTERFACE:equilibrium::base>
  $<$<PLATFORM_ID:Linux>:pthread>)

if(ANDROID)
  # log.cc writes to logcat.
  target_link_libraries(rx_foundation PUBLIC log)
endif()
