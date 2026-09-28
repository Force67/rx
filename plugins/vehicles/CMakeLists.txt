# rx::vehicles: cars (profiles for PhysicsWorld's wheeled vehicle), boats,
# fixed-wing aircraft and kites, plus the procedural engine/skid/wind audio that
# rides their telemetry. The simulators use only PhysicsWorld's public force API,
# so they build against the stub too and are inert there (valid() == false)
# rather than missing, which keeps every consumer linking without Jolt.
rx_add_module(vehicles
  vehicle_profiles.cc
  boat.cc
  boat_profiles.cc
  aircraft.cc
  kite.cc
  engine_synth.cc
  aux_synth.cc
  vehicle_audio.cc
)
target_link_libraries(rx_vehicles PUBLIC rx::physics rx::audio rx::asset rx::foundation)
