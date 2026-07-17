// Cross-system vehicle integration test, fully headless (no GPU, no audio
// device): one PhysicsWorld hosts a car, a boat and an aircraft simultaneously
// for ~30 simulated seconds at a 60 Hz fixed step. The world is a flat asphalt
// heightfield (the land) with a Gerstner water callback fenced into one far
// quadrant (the lake); the boat floats there while the car drives and the plane
// takes off on the land. Every vehicle's telemetry is mapped to SynthParams and
// rendered through a real EngineSynth each frame. The test asserts (a) the car
// drives + shifts with in-range telemetry, (b) the boat throttles forward and
// stays afloat NaN-free, (c) the plane climbs, (d) all three audio streams stay
// finite/bounded/non-silent with the car synth responding to a load change, and
// (e) that three simulators sharing one world do not corrupt each other.
//
// The vehicle Update-before-PhysicsWorld::Update contract is honoured: every
// simulator stages its forces for the frame, then the world integrates once.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "audio/engine_synth.h"
#include "audio/synth_voice.h"
#include "core/math.h"
#include "physics/aircraft.h"
#include "physics/boat.h"
#include "physics/physics_world.h"
#include "physics/water_waves.h"

using namespace rx;
using audio::EnginePreset;
using audio::EngineSynth;
using audio::SynthParams;
using physics::Aircraft;
using physics::AircraftDesc;
using physics::AircraftInput;
using physics::Boat;
using physics::BoatDesc;
using physics::BoatInput;
using physics::PhysicsWorld;
using physics::SurfaceType;
using physics::VehicleId;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
constexpr u32 kRate = 48000;
constexpr u32 kBlock = kRate / 60;        // 800 samples per fixed step
constexpr int kSteps = 60 * 30;           // 30 simulated seconds

// The lake sits far out in +X so it never overlaps the land heightfield: water
// is reported only past kLakeEdge, the land (car + plane) is always dry.
constexpr f32 kLakeEdge = 1100.0f;
constexpr f32 kLakeX = 1500.0f;

// Wave clock, advanced once per fixed step before the simulators sample.
f32 g_wave_t = 0.0f;

int Fail(const char* what) {
  std::fprintf(stderr, "vehicle_integration_test FAIL: %s\n", what);
  return 1;
}

bool IsFinite(const Vec3& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// --- audio buffer helpers (mirror vehicle_audio_test) ---------------------
bool AllFinite(const std::vector<f32>& b) {
  for (f32 v : b)
    if (!std::isfinite(v)) return false;
  return true;
}
f32 MaxAbs(const std::vector<f32>& b) {
  f32 m = 0.0f;
  for (f32 v : b) m = std::max(m, std::fabs(v));
  return m;
}
// RMS over the half-open sample window [lo, hi).
f32 RmsWindow(const std::vector<f32>& b, size_t lo, size_t hi) {
  hi = std::min(hi, b.size());
  if (lo >= hi) return 0.0f;
  f64 acc = 0.0;
  for (size_t i = lo; i < hi; ++i) acc += static_cast<f64>(b[i]) * b[i];
  return static_cast<f32>(std::sqrt(acc / static_cast<f64>(hi - lo)));
}

// World up projected onto the boat's up axis: 1 = upright, 0 = on its side.
f32 Uprightness(const Quat& q) { return Rotate(q, Vec3{0, 1, 0}).y; }

}  // namespace

int main() {
  PhysicsWorld probe;
  if (!probe.Initialize()) {
    std::fprintf(stderr, "vehicle_integration_test: physics unavailable, skipping\n");
    return 0;
  }

  PhysicsWorld world;
  world.Initialize();

  // Land: one big flat asphalt heightfield centred on the origin. It stops well
  // short of the lake, so the boat floats over open water with no floor beneath.
  {
    constexpr u32 kSamples = 64;
    constexpr f32 kSize = 2000.0f;
    std::vector<f32> heights(static_cast<size_t>(kSamples) * kSamples, 0.0f);
    world.AddHeightField(Vec3{-kSize * 0.5f, 0.0f, -kSize * 0.5f}, heights.data(), kSamples, kSize,
                         SurfaceType::kAsphalt);
  }

  // Gerstner lake fenced into the far +X quadrant; dry land everywhere else.
  world.set_water_height([](const Vec3& p, f32* h, Vec3* flow) {
    if (p.x < kLakeEdge) return false;  // land: no water under the car/plane
    Vec3 f{};
    *h = physics::GerstnerWaveHeight(p.x, p.z, g_wave_t, &f, nullptr);
    *flow = f;
    return true;
  });

  // --- spawn all three, well separated in X so nobody collides ---
  PhysicsWorld::VehicleDesc car_desc;
  car_desc.drivetrain = PhysicsWorld::Drivetrain::kAWD;  // reliable launch traction
  const VehicleId car = world.CreateVehicle(car_desc, Vec3{-40.0f, car_desc.wheel_radius + 0.6f, 0.0f}, 0.0f);
  if (car == 0) return Fail("car spawn failed");

  Boat boat(world, BoatDesc{}, Vec3{kLakeX, 0.5f, 0.0f}, 0.0f);
  if (!boat.valid()) return Fail("boat spawn failed");

  AircraftDesc plane_desc;
  plane_desc.payload_kg = 120.0f;  // light load (solo, low fuel): a healthy climb
  Aircraft plane(world, plane_desc, Vec3{40.0f, 1.7f, 0.0f}, 0.0f);
  if (!plane.valid()) return Fail("aircraft spawn failed");

  // --- settle: 2.5 s of zero input so the gear/suspension/hull all quiet ---
  for (int i = 0; i < 150; ++i) {
    world.DriveVehicle(car, 0, 0, 0, 0);
    boat.Update(BoatInput{}, kDt);
    plane.Update(AircraftInput{}, kDt);
    world.Update(kDt);
  }

  const f32 plane_ground_y = plane.state().position.y;
  Vec3 car_start{};
  {
    f32 rot[4];
    world.GetVehicleTransform(car, &car_start, rot);
  }

  // Three engines rendered straight from telemetry (no device, no mixer).
  EngineSynth car_synth(audio::InlineFourCarPreset(), kRate);
  EngineSynth boat_synth(audio::InboardBoatPreset(), kRate);
  EngineSynth plane_synth(audio::SinglePropPlanePreset(), kRate);
  std::vector<f32> car_audio(static_cast<size_t>(kSteps) * kBlock, 0.0f);
  std::vector<f32> boat_audio(static_cast<size_t>(kSteps) * kBlock, 0.0f);
  std::vector<f32> plane_audio(static_cast<size_t>(kSteps) * kBlock, 0.0f);

  // Car audio load-response windows: an early idle stretch vs a later
  // full-throttle stretch (throttle schedule below drives the change).
  constexpr int kIdleEnd = 120;   // frames 0..119: car idling (< 2 s)
  const size_t idle_lo = static_cast<size_t>(60) * kBlock;
  const size_t idle_hi = static_cast<size_t>(120) * kBlock;
  const size_t load_lo = static_cast<size_t>(360) * kBlock;
  const size_t load_hi = static_cast<size_t>(660) * kBlock;

  int car_max_gear = 0;
  f32 car_max_speed = 0.0f;
  f32 car_redline = car_desc.max_rpm > 0 ? car_desc.max_rpm : 6000.0f;
  f32 boat_max_fwd = 0.0f;
  f32 plane_max_alt = 0.0f;
  f32 plane_peak_climb = 0.0f;

  // --- the shared 30 s run: stage all three, then step the world once ---
  for (int frame = 0; frame < kSteps; ++frame) {
    g_wave_t += kDt;

    // (a) Car: idle briefly, then full throttle with a mild weave.
    const f32 car_throttle = frame < kIdleEnd ? 0.0f : 1.0f;
    const f32 car_steer = frame < kIdleEnd ? 0.0f : 0.25f * std::sin(frame * 0.02f);
    world.DriveVehicle(car, car_throttle, car_steer, 0.0f, 0.0f);

    // (b) Boat: straight-ahead throttle across the chop.
    BoatInput boat_in;
    boat_in.throttle = 1.0f;
    boat.Update(boat_in, kDt);

    // (c) Aircraft: full power. Accelerate on the ground to rotation speed, then
    // fly a classic speed-hold climb: elevator trims to a target airspeed and
    // the excess power at full throttle turns into a steady climb. Holding
    // airspeed this way damps the phugoid a fixed elevator would porpoise on.
    const physics::AircraftState& pre = plane.state();
    AircraftInput plane_in;
    plane_in.throttle = 1.0f;
    plane_in.pitch = pre.airspeed_mps > 26.0f ? 0.7f : 0.0f;  // rotate at Vr, hold up
    plane.Update(plane_in, kDt);

    world.Update(kDt);

    // --- read telemetry back ---
    PhysicsWorld::VehicleState cst;
    if (!world.GetVehicleState(car, &cst)) return Fail("(a) car telemetry read failed");
    if (cst.rpm < 0.0f || cst.rpm > car_redline + 50.0f) return Fail("(a) car rpm out of range");
    if (cst.engine_load < 0.0f || cst.engine_load > 1.0f) return Fail("(a) car load out of [0,1]");
    car_max_gear = std::max(car_max_gear, cst.gear);
    const f32 car_speed = world.VehicleForwardSpeed(car);
    car_max_speed = std::max(car_max_speed, car_speed);
    f32 car_slip = 0.0f;
    for (u32 w = 0; w < cst.wheel_count; ++w)
      car_slip = std::max(car_slip, cst.wheels[w].longitudinal_slip);

    const physics::BoatState& bst = boat.state();
    boat_max_fwd = std::max(boat_max_fwd, bst.forward_speed);

    const physics::AircraftState& ast = plane.state();
    plane_max_alt = std::max(plane_max_alt, ast.position.y - plane_ground_y);
    if (!ast.on_ground) plane_peak_climb = std::max(plane_peak_climb, ast.vertical_speed_mps);

    // Per-frame NaN gate: three simulators must never poison the shared world.
    if (!std::isfinite(car_speed) || !std::isfinite(cst.rpm)) return Fail("(a) car NaN in telemetry");
    if (!IsFinite(bst.position) || !std::isfinite(bst.rpm) || !std::isfinite(bst.forward_speed) ||
        !std::isfinite(bst.rotation.w))
      return Fail("(b) boat NaN in telemetry");
    if (!IsFinite(ast.position) || !std::isfinite(ast.airspeed_mps) ||
        !std::isfinite(ast.vertical_speed_mps) || !std::isfinite(ast.rotation.w))
      return Fail("(c) aircraft NaN in telemetry");

    // (d) Telemetry -> SynthParams -> one rendered block per vehicle.
    SynthParams cp;
    cp.rpm = cst.rpm;
    cp.load = cst.engine_load;
    cp.throttle = car_throttle;
    cp.speed_mps = std::fabs(car_speed);
    cp.slip = car_slip;
    car_synth.Render(&car_audio[static_cast<size_t>(frame) * kBlock], kBlock, cp);

    SynthParams bp;
    bp.rpm = bst.rpm;
    bp.load = bst.engine_load;
    bp.throttle = std::max(0.0f, bst.throttle);
    bp.speed_mps = bst.speed_mps;
    boat_synth.Render(&boat_audio[static_cast<size_t>(frame) * kBlock], kBlock, bp);

    SynthParams pp;
    pp.rpm = ast.rpm;
    pp.load = ast.engine_load;
    pp.throttle = ast.throttle;
    pp.speed_mps = ast.airspeed_mps;
    plane_synth.Render(&plane_audio[static_cast<size_t>(frame) * kBlock], kBlock, pp);
  }

  // ---------------------------------------------------------------------------
  // (a) The car moved, the automatic box shifted, telemetry stayed in range.
  {
    Vec3 pos{};
    f32 rot[4];
    world.GetVehicleTransform(car, &pos, rot);
    const f32 dx = pos.x - car_start.x, dz = pos.z - car_start.z;
    const f32 travelled = std::sqrt(dx * dx + dz * dz);
    std::fprintf(stderr, "(a) car: travelled=%.1f m max_speed=%.1f m/s max_gear=%d\n", travelled,
                 car_max_speed, car_max_gear);
    if (travelled < 20.0f) return Fail("(a) car did not drive");
    if (car_max_speed < 10.0f) return Fail("(a) car did not reach speed");
    if (car_max_gear < 2) return Fail("(a) automatic box did not shift past 1st");
  }

  // (b) The boat throttled forward and is still afloat.
  const physics::BoatState& bfin = boat.state();
  std::fprintf(stderr, "(b) boat: max_fwd=%.1f m/s pos=(%.0f,%.2f,%.1f) wetted=%.2f up=%.2f\n",
               boat_max_fwd, bfin.position.x, bfin.position.y, bfin.position.z, bfin.wetted,
               Uprightness(bfin.rotation));
  if (boat_max_fwd < 3.0f) return Fail("(b) boat did not accelerate forward");
  if (bfin.position.z < 10.0f) return Fail("(b) boat did not travel forward (+Z)");
  if (bfin.wetted <= 0.0f) return Fail("(b) boat sank (no submerged samples)");
  if (bfin.position.y < -1.0f) return Fail("(b) boat dropped below the surface");

  // (c) The aircraft climbed away from the runway.
  const physics::AircraftState& afin = plane.state();
  std::fprintf(stderr,
               "(c) aircraft: max_alt_gain=%.1f m peak_climb=%.2f m/s airspeed=%.1f m/s "
               "on_ground=%d\n",
               plane_max_alt, plane_peak_climb, afin.airspeed_mps, afin.on_ground);
  if (plane_max_alt < 10.0f) return Fail("(c) aircraft did not gain altitude");

  // (d) All three audio streams: finite, bounded, non-silent; car responds to load.
  const f32 car_peak = MaxAbs(car_audio), boat_peak = MaxAbs(boat_audio),
            plane_peak = MaxAbs(plane_audio);
  const f32 car_idle_rms = RmsWindow(car_audio, idle_lo, idle_hi);
  const f32 car_load_rms = RmsWindow(car_audio, load_lo, load_hi);
  std::fprintf(stderr,
               "(d) audio peaks car=%.3f boat=%.3f plane=%.3f | car rms idle=%.4f load=%.4f\n",
               car_peak, boat_peak, plane_peak, car_idle_rms, car_load_rms);
  if (!AllFinite(car_audio) || !AllFinite(boat_audio) || !AllFinite(plane_audio))
    return Fail("(d) audio produced a non-finite sample");
  if (car_peak > 1.001f || boat_peak > 1.001f || plane_peak > 1.001f)
    return Fail("(d) audio exceeded the [-1,1] soft-limit bound");
  if (car_peak < 0.02f || boat_peak < 0.02f || plane_peak < 0.02f)
    return Fail("(d) an audio stream was silent while running");
  if (car_load_rms <= car_idle_rms * 1.2f)
    return Fail("(d) car synth did not respond to the idle->full-throttle load change");

  // (e) Cross-interference: after 30 s of sharing one world, everyone is sane.
  {
    PhysicsWorld::VehicleState cst;
    if (!world.GetVehicleState(car, &cst)) return Fail("(e) car telemetry gone");
    Vec3 cpos{};
    f32 rot[4];
    world.GetVehicleTransform(car, &cpos, rot);
    bool car_grounded = false;
    for (u32 w = 0; w < cst.wheel_count; ++w) car_grounded |= cst.wheels[w].contact;
    if (!IsFinite(cpos)) return Fail("(e) car position NaN");
    if (!car_grounded) return Fail("(e) car left the ground");
    if (std::fabs(cpos.y) > 5.0f) return Fail("(e) car position drifted off the surface");

    if (!IsFinite(bfin.position)) return Fail("(e) boat position NaN");
    if (bfin.wetted <= 0.0f) return Fail("(e) boat no longer afloat");
    if (Uprightness(bfin.rotation) < 0.5f) return Fail("(e) boat capsized");

    if (!IsFinite(afin.position)) return Fail("(e) aircraft position NaN");
  }

  std::fprintf(stderr, "vehicle_integration_test: all checks passed\n");
  return 0;
}
