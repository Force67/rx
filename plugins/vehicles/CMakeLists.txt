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

if(RX_BUILD_TESTS)
  # Procedural vehicle audio: engine/skid/wind synthesis, the SynthVoice seam and
  # the VehicleAudio driver through the mixer. Pure CPU (no device), always runs.
  add_executable(vehicle_audio_test vehicle_audio_test.cc)
  target_link_libraries(vehicle_audio_test PRIVATE rx::vehicles rx::audio rx::foundation)
  add_test(NAME vehicle_audio_test COMMAND vehicle_audio_test)
endif()

if(RX_BUILD_TESTS AND RX_JOLT)
  # Physics-driven fixed-wing aircraft over the public PhysicsWorld force API:
  # ground roll + rotation + climb, overloaded (over-MTOM) degradation, stall
  # sink, flap-shortened takeoff, braking, rudder yaw, and NaN-free tumbling.
  # Headless (no GPU), needs the Jolt-enabled physics build like vehicle_test.
  add_executable(aircraft_test aircraft_test.cc)
  target_link_libraries(aircraft_test PRIVATE rx::vehicles rx::physics rx::foundation)
  add_test(NAME aircraft_test COMMAND aircraft_test)

  # Cross-system vehicle integration: one PhysicsWorld hosts a car, a boat and an
  # aircraft at once for 30 s over a shared heightfield + Gerstner lake, with every
  # vehicle's telemetry driving a real EngineSynth. Headless (no GPU/device), needs
  # Jolt for the physics path (like aircraft_test/vehicle_test/boat_test).
  add_executable(vehicle_integration_test vehicle_integration_test.cc)
  target_link_libraries(vehicle_integration_test PRIVATE rx::vehicles rx::physics rx::audio rx::foundation)
  add_test(NAME vehicle_integration_test COMMAND vehicle_integration_test)

  # Handling profiles: measured, ordered proof that the six presets
  # (vehicle_profiles.h) drive differently - 0-100, braking,
  # roll, grass launch, under/oversteer and downforce. Headless, needs Jolt.
  add_executable(handling_test handling_test.cc)
  target_link_libraries(handling_test PRIVATE rx::vehicles rx::physics rx::foundation)
  add_test(NAME handling_test COMMAND handling_test)

  # Physics-driven boat over the force-based hull model: floating draft,
  # acceleration, steering, drag decay, planing, river drift, self-righting,
  # buoyancy-exemption and NaN-free chop. Headless (no GPU), needs Jolt.
  add_executable(boat_test boat_test.cc)
  target_link_libraries(boat_test PRIVATE rx::vehicles rx::physics rx::foundation)
  add_test(NAME boat_test COMMAND boat_test)

  # Boat-type profiles + cargo load: measured, ordered proof that the five presets
  # (boat_profiles.h) float and handle differently and that cargo
  # sinks the hull deeper, slows accel/turning, delays planing and eats freeboard
  # - settle-draft/top-speed/turn-rate orderings, capsize vs self-right, an
  # overloaded barge near awash, and NaN-free chop. Headless, needs Jolt.
  add_executable(boat_profiles_test boat_profiles_test.cc)
  target_link_libraries(boat_profiles_test PRIVATE rx::vehicles rx::physics rx::foundation)
  add_test(NAME boat_profiles_test COMMAND boat_profiles_test)

  # Physics-driven tethered kite over the force-based flat-plate sail model:
  # launch + stable overhead equilibrium in steady wind, descent + settle in dead
  # air, left/right steering displacement, reel-in altitude change, tow-launch on
  # apparent wind and NaN-free / tension-capped gusting. Headless, needs Jolt.
  add_executable(kite_test kite_test.cc)
  target_link_libraries(kite_test PRIVATE rx::vehicles rx::physics rx::foundation)
  add_test(NAME kite_test COMMAND kite_test)
endif()
