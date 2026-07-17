# Vehicles

Three vehicle simulators layered over the public `rx::physics::PhysicsWorld`
(Jolt) force API, plus a procedural audio stack that renders each engine from
telemetry. Nothing here is bespoke Jolt code: the boat and aircraft integrate
their own hydro/aero models on top of `AddForce` / `AddForceAtPoint` /
`AddTorque` / `SampleWater`, and the car is Jolt's `WheeledVehicleController`
with a surface-aware tyre-friction combine bolted on. Gravity, integration and
contact response stay in Jolt.

Shared `PhysicsWorld` primitives the simulators lean on: a global
`set_wind(Vec3)` / `wind()` (uniform airmass velocity, m/s world space, default
still air) the aero models sample; a `Raycast(origin, dir, max, out, ignore)`
overload that skips one body, so a suspension ray cast from a hardpoint *inside*
a body's own collision shape does not hit itself (`ignore == 0` skips nothing);
and `SetBodyInertia(id, diagonal_kgm2)` to replace a dynamic body's
collision-shape-derived inertia tensor with an explicit body-space diagonal.

- **Cars / motorcycles** — `PhysicsWorld::CreateVehicle` /
  `CreateMotorcycle` (`engine/physics/physics_world.{h,cc}`).
- **Boats** — `rx::physics::Boat` (`engine/physics/boat.h`).
- **Aircraft** — `rx::physics::Aircraft` (`engine/physics/aircraft.h`).
- **Audio** — `rx::audio` engine/skid/wind synthesis and the `VehicleAudio`
  driver (`engine/audio/{engine_synth,synth_voice,aux_synth,vehicle_audio}.h`).

All SI units (metres, kg, seconds, newtons, radians), engine convention: **+Z
forward, +Y up, right-handed**, so the RIGHT side is −X.

## Update-ordering contract

The force-based simulators (boat, aircraft) only *stage* forces on their body;
Jolt clears accumulated forces after it integrates. So each fixed step:

```
for each vehicle:  vehicle.Update(input, dt)   // stage this step's forces
world.Update(dt)                               // Jolt integrates once
```

`vehicle.Update` **must** run before `PhysicsWorld::Update`, with the same `dt`,
which must be the world's fixed step (~1/60 s). Telemetry (`state()`) is
refreshed from the body pose sampled at the *start* of the step. The Jolt car is
driven the mirror way: `DriveVehicle(...)` then `world.Update(dt)`. Cross-system
proof that all three coexist in one world: `test/vehicle_integration_test.cc`.

---

## Cars & motorcycles

Jolt `VehicleConstraint` + `WheeledVehicleController` (cars) or
`MotorcycleController` (bikes). A dynamic chassis box on four (or two)
suspension-raycast wheels with an automatic or manual gearbox. Wheel order:
cars `FL FR RL RR` (front steer, rear handbrake), bikes `front rear`. There is
no wheel geometry in the physics world — wheels are suspension raycasts; the
caller renders chassis/wheels from `GetVehicleTransform` / `GetVehicleWheel`.

The racing-sim fields on `VehicleDesc` all default to "keep the legacy arcade
tune" (`0` or `-1` = the previous hardcoded/Jolt default), so existing consumers
are unchanged.

### `VehicleDesc` (drivetrain highlights)

| Field | Meaning |
| --- | --- |
| `drivetrain` | `kRWD` / `kFWD` / `kAWD`; `awd_front_split` sets the front torque fraction |
| `gear_count`, `gear_ratios[8]`, `final_drive` | manual box; `0` keeps Jolt's 5-speed |
| `max_rpm`, `min_rpm` | `max_rpm` doubles as the hard rev limiter |
| `shift_up_rpm`, `shift_down_rpm` | automatic shift points (`0` = 4000 / 2000) |
| `engine_inertia` | flywheel kg·m² (reflects as ratio²·inertia through the box) |
| `clutch_strength` | clutch coupling (`0` = legacy 10) |
| `engine_braking` | off-throttle engine angular damping (trailing-throttle decel) |
| `torque_curve[8]`, `torque_curve_count` | normalized (rpm-frac, torque-frac) curve; `0` = Jolt's stock 0.8/1.0/0.8 |
| `tire_long_friction`, `tire_lat_friction` | scale the tyre slip-curve peaks (`~1.5` = slicks) |
| `front_lat_friction`, `rear_lat_friction` | per-axle lateral grip (`0` = fall back to `tire_lat_friction`); front < rear = understeer, rear < front = oversteer |
| `front_suspension_frequency`, `rear_suspension_frequency` | per-axle spring rate (`0` = `suspension_frequency` then Jolt default) |
| `downforce`, `downforce_balance` | `F = downforce · v²`; `balance` (`0.5` = at the CoM) presses that fraction at the front axle, the rest at the rear |
| `limited_slip_ratio` | Jolt LSD max/min driven-wheel speed ratio on every driven diff (`0` = default `1.4`, lower = tighter lock) |
| `anti_roll_front`, `anti_roll_rear` | per-axle anti-roll bar stiffness (N/m); each falls back to `anti_roll_stiffness` then Jolt's default |
| `brake_bias_front` | fraction of the per-wheel brake torque to the front axle (`0.5` = even; road cars ~`0.6`) |
| `steer_high_speed_fraction`, `steer_fade_speed` | steering slows from full at rest to `fraction` by `fade_speed` m/s (`fraction ≥ 1` or `fade_speed ≤ 0` = no fade) |
| `com_fore` | CoM shift along +Z (forward); negative = rearward (laden van/truck) |
| `traction_control` | cuts throttle past ~8% driven-wheel slip; applied in both the automatic and manual (`DriveVehicle(VehicleInput)`) paths |
| `free_rolling` | unpowered coasting chassis (a towed trailer/carriage): the engine is disconnected (zero max torque, zero engine braking, zero idle RPM) while a valid differential is kept so Jolt's driven-torque-sum-to-1 invariant holds. Throttle does nothing; the front axle still steers and the rear still takes the handbrake. Tow it with `AddForceAtPoint` on `GetVehicleBody` |

Each new field is additive with a default that reproduces the previous
behaviour, and each maps to a real Jolt vehicle knob (`VehicleDifferentialSettings`,
`VehicleAntiRollBar`, per-wheel `mMaxBrakeTorque`, `OffsetCenterOfMass`); the
steering fade and the axle-split downforce are applied in the rx layer around the
constraint.

`MotorcycleDesc` adds a lean spring (`lean_spring` / `lean_damping` /
`max_lean_angle`) and a speed-aware steer limit so the bike banks into corners
without toppling; handbrake input is ignored.

### Manual box

`SetManualTransmission(id, true)` switches to the `VehicleInput` overload:
`shift_up` / `shift_down` are edges (a rising edge changes one gear), and the
`clutch` input (`0..1`, `1` = fully disengaged) is honoured. In automatic mode
the box works the clutch itself and the shift edges are ignored.

### Telemetry — `VehicleState`

`rpm`, `gear` (Jolt convention: `-1` reverse, `0` neutral, `1..N`),
`forward_speed` (signed m/s), `engine_load` (`0..1` = delivered/available
torque), `engine_torque` (Nm), `is_shifting` (clutch slipping through a change),
and per-wheel `WheelState`: `contact`, `suspension_length`,
`suspension_compression` (`0..1`), `longitudinal_slip` (`0` traction .. `1`
locked/spinning), `angular_velocity`, `rotation_angle`, `surface`, `submerged`,
`wading_depth`.

### Surface material system

Static colliders carry a `SurfaceType` tag; the tyre-friction combine looks it
up per wheel contact. Untagged geometry defaults to `kAsphalt` (full grip, the
legacy feel).

| API | Effect |
| --- | --- |
| `AddStaticBox/Mesh/Shape/HeightField(..., SurfaceType)` | tag a whole collider |
| `AddHeightField(..., material_indices, palette, palette_count)` | per-quad materials: `(samples-1)²` indices into a palette, so one cell carries an asphalt road over grass |
| `AddStaticMeshInstance(..., SurfaceType)` | shared shape, surface recorded per body and resolved at contact |

Instead of Jolt's default `sqrt(tyre, body)` combine, the callback scales the
tyre's own longitudinal/lateral grip by the surface's table entry × rain wetness
× per-wheel aquaplaning. Grip table (`physics_world.cc`, a documented tune, not
measured — `longitudinal` / `lateral` scale the tyre peak, `1` = dry asphalt;
`wet` is the multiplier at full rain, lerped from `1` dry):

| Surface | long | lat | wet |
| --- | --- | --- | --- |
| asphalt  | 1.00 | 1.00 | 0.70 |
| concrete | 0.98 | 0.98 | 0.75 |
| dirt     | 0.75 | 0.72 | 0.55 |
| gravel   | 0.80 | 0.75 | 0.70 |
| grass    | 0.65 | 0.60 | 0.55 |
| sand     | 0.55 | 0.50 | 0.60 |
| snow     | 0.45 | 0.40 | 0.80 |
| ice      | 0.15 | 0.12 | 0.70 |
| mud      | 0.45 | 0.40 | 0.85 |
| wood     | 0.85 | 0.82 | 0.65 |
| metal    | 0.70 | 0.65 | 0.55 |

### Wetness & aquaplaning

`set_surface_wetness(0..1)` is global rain: it lerps every surface's grip toward
its `wet` value (wet asphalt ~0.7 of dry; loose dirt turns mud-like). Off (`0`)
by default.

Aquaplaning is per-wheel and needs standing water (the `set_water_height`
callback returning true at the contact). Grip fades as the contact patch floods
and speed builds a water wedge the tread can't clear:
`grip ·= 1 − 0.9 · depth_frac · speed_frac`, where a patch is "fully awash" at
half the wheel radius of water, onset is ~8 m/s and full hydroplaning ~25 m/s.
Below onset or on a dry patch it is a no-op.

The `set_water_height` callback is **game-thread only**: it is evaluated inside
`Update` (before the Jolt step) and from `SampleWater`, never from a Jolt worker
thread. Each wheel's water is sampled on the game thread into a per-vehicle cache
that the in-step tyre-friction callback reads, so a callback backed by
thread-affine terrain data or physics queries is safe. The cache uses the
previous step's wheel positions — one step of latency on the slow aquaplaning
ramp.

---

## Handling profiles

`engine/physics/vehicle_profiles.h` ships six fully-tuned `VehicleDesc` presets
so a truck handles like a truck and a sports car like a sports car out of
`CreateVehicle`, GTA-style. Each is a free function returning a complete desc
(dimensions, mass, wheel geometry, drivetrain, engine, gearbox, suspension,
tyres, aero and the handling-profile fields above); its doc comment states the
intended signature in one line.

| Profile | Mass | Layout | Signature |
| --- | --- | --- | --- |
| `SportsCarProfile` | 1300 kg | RWD | low CG, stiff + strong front anti-roll, fast steering with a hard high-speed fade, sticky tyres, downforce, short gears — eager, planted at speed |
| `MuscleCarProfile` | 1650 kg | RWD | big low-end torque, softer rear, modest anti-roll, rear grip below front — tail-happy power oversteer |
| `HatchbackProfile` | 1150 kg | FWD | economical, understeer bias (front grip below rear), soft-ish, light — safe and nose-led |
| `SuvProfile` | 2100 kg | AWD 30/70 | tall body, soft long-travel suspension, mild anti-roll, all-terrain tyres — sure-footed launch, leans in corners |
| `VanProfile(cargo_load)` | 2000–2400 kg | RWD | high CG over a narrow-ish track, slow steering; `cargo_load` (0..1) adds mass and shifts the CoM rearward/up (`com_fore`) |
| `SemiTruckProfile` | 8500 kg | RWD | enormous torque geared tall, very slow steering, weak per-kg brakes (long stops), pronounced roll, gearing-capped top speed |

A Jolt-auto-box note: with these masses and gearings the automatic transmission
will refuse to upshift out of a gear pinned at its redline (it only shifts while
the engine is still revving up and the driven wheels grip). The presets work
around it with revvier redlines than a real diesel and, on the FWD hatch, a hard
top-end torque taper — so every profile reaches 100 km/h under the stock
automatic box.

### Measured behaviour

From `test/handling_test.cc` (headless, flat asphalt, real Jolt path — the test
asserts the *orderings*, not the absolute numbers):

| Profile | 0–100 km/h | 100–0 km/h | step-steer roll¹ |
| --- | --- | --- | --- |
| Sports | 5.98 s | 25.4 m | 0.2° |
| Muscle | 8.83 s | 53.4 m | — |
| Hatchback | 17.20 s | 36.4 m | — |
| SUV | 21.33 s | 40.8 m | 0.5° |
| Van (laden) | 36.33 s | 41.8 m | 3.5° |
| Semi | 80.43 s | 75.6 m | 1.2° |

¹ Peak body-lean in a gentle step-steer at ~55 km/h. At 80 km/h the soft, tall
profiles roll fully onto their side (a saturated ~60°), so the graded lean
ordering (sports < SUV < van/semi) is only measurable below the rollover regime.

Further proven orderings: on **Grass** the AWD SUV reaches 40 km/h in 4.23 s vs
the RWD muscle car's 5.05 s (traction); at full-throttle mid-corner the **FWD
hatch's front axle slips more than its rear** (understeer: 0.307 vs 0.146 rad)
while the **RWD muscle car's rear exceeds its front** (oversteer: 1.457 vs 1.421
rad); and the sports car's achievable lateral grip **grows with speed** thanks to
downforce (×1.67 from 60→120 km/h) where the hatch's barely moves (×1.30). All
six are NaN-free, settle at rest, and still respect the surface/wetness grip
table.

### Live in the demo

Number keys **1–6** swap the active car's handling profile
(sports/muscle/hatchback/SUV/van/semi) — the car despawns and respawns with the
new desc at its current position and heading (stationary), the HUD shows the
profile name and mass, and the visual switches per profile: the box-shaped
**van/semi wear the milk-truck glTF** (scaled to the desc length) while the
sportier profiles get a **tinted graybox chassis** scaled to the desc dimensions
(a milk truck would look absurd as a coupe). `RX_DRIVE_PROFILE=sports|muscle|
hatch|suv|van|semi` forces the initial profile for headless captures.

---

## Boats

`rx::physics::Boat` — a force-based motorboat over one dynamic hull body. The
constructor spawns the hull and **exempts it from the world's generic
buoyancy** (`set_buoyancy_exempt`) so only the hull model's forces act; the two
schemes never stack. `BoatInput` is `throttle` (`-1..1`), `steer` (`-1..1`),
`trim` (`-1..1`).

Force models (`BoatDesc`):

| Model | Summary |
| --- | --- |
| Volumetric buoyancy grid | `grid_beam × grid_height × grid_len` samples fill the hull box; each displaces its share while submerged, so the centre of buoyancy shifts to the low side on heel (righting) and aft on bow-lift (pitch stability) — true metacentric behaviour, one `SampleWater` call per sample. `grid_height ≥ 2` self-rights a knock-down. |
| Ballast keel (`com_drop`) | effective CoM below the geometric centre, emulated as a gravity righting couple (`τ = r_ballast × m·g`) since Jolt keeps the real CoM at the box centre |
| Hull drag | quadratic, wetted-fraction scaled, taken relative to water flow: `drag_fwd` (streamlined bow) / `drag_aft` (blunt transom) / `drag_lateral` (keel carves) / `drag_vertical` (heave) |
| Planing | past `hull_speed` the bow lifts and longitudinal drag drops by `plane_drag_reduction`, so a planing hull tops out faster; `plane_lift` capped at `plane_lift_cap × weight` |
| Propeller | `max_thrust` vs a spooling `rpm` (`spool_time` lag), applied at `prop_offset` **only while that point is submerged** — launch off a wave and the screw loses bite; `reverse_fraction` astern |
| Rudder | stern side force `= steer · (rudder_speed_gain · v_fwd² + rudder_wash_gain · |thrust|)`, so prop wash gives steering authority at a standstill; `yaw_damping` settles turns |
| River flow | all drag/thrust is relative to the `SampleWater` flow, so a current carries the hull |
| Wind | the world `wind()` pushes on the exposed (above-water) topsides: force quadratic in the wind speed *relative to the hull*, scaled by the exposed `1 − submerged` fraction and a directional blend of the box side/bow areas, applied above the waterline so a strong beam wind shoves a drifting hull downwind and heels it slightly; conservative `wind_drag` coefficient |

### Telemetry — `BoatState`

`rpm`, `engine_load` (`0..1`, spool-limited thrust fraction), `throttle`,
`speed_mps`, `forward_speed` (signed, hull axis), `planing` (`0..1`), `wetted`
(`0..1` submerged-sample fraction), `draft_m` (submerged hull depth, waterline −
hull bottom, from the grid's submerged fraction so it tracks load and swell),
`freeboard_m` (hull depth left above the waterline), `cargo_kg` (current load),
`prop_submerged`, `position`, `rotation`.

### Boat profiles & cargo

![empty barge](media/vehicles/barge_empty.jpg)
![overloaded barge](media/vehicles/barge_laden.jpg)


`engine/physics/boat_profiles.h` ships five fully-tuned `BoatDesc` presets so a
dinghy handles like a dinghy and a work barge like a barge straight out of the
`Boat` constructor — the boat-side mirror of the [handling profiles](#handling-profiles).
Each is a free function returning a complete desc (hull dims, mass, engine/prop,
drag/planing/righting tune and cargo capacity) with a one-line signature comment.

| Profile | Length | Mass | Cargo cap. | Signature |
| --- | --- | --- | --- | --- |
| `DinghyProfile` | ~3 m | 220 kg | 200 kg | light, twitchy, planes early, low top speed; shallow ballast → capsizes far more easily |
| `SpeedboatProfile` | ~6 m | 1400 kg | 1600 kg | fast, planes hard, agile — the benchmark (equals the `BoatDesc` defaults) |
| `JetskiProfile` | ~2.2 m | 350 kg | 120 kg | extreme thrust-to-weight, planes almost at once, spins on a dime, easily flipped but self-rights |
| `FishingBoatProfile` | ~9 m | 6500 kg | 5000 kg | heavy displacement hull, high drag, very stable (strong ballast); barely planes even empty |
| `WorkBargeProfile` | ~12 m | 12000 kg | 31000 kg | very heavy, huge capacity, enormous draft when laden, slow big-torque engine, wide turning; never planes |

**Cargo / draft model.** `BoatDesc` gains `max_cargo_kg`, a `cargo_overload_fraction`
(SetCargo clamps to `max_cargo_kg × fraction`, so an overloaded boat still floats
— like the aircraft's over-MTOM spawn — with little reserve) and a
`cargo_com_offset`. `Boat::SetCargo(kg)` applies the load at runtime through the
new `PhysicsWorld::SetBodyMass(BodyId, kg)` primitive (Jolt inverse-mass +
proportionally scaled inverse-inertia, stub-mirrored). Every consequence is
**emergent, not scripted**: the buoyancy grid displaces the same volume, so more
weight settles the hull deeper (draft) until it rebalances; the extra mass *and*
inertia slow acceleration and turning; the deeper hull stays wetted so it planes
later or never; and `cargo_com_offset.y` (cargo above the centre) shrinks the
effective ballast lever, cutting the self-righting margin. The structural limit
sits close to unstable by design — a `1.25×` barge floats in calm water with its
deck near awash and feels precarious in chop.

**Measured behaviour** (from `test/boat_profiles_test.cc`, headless, flat water,
real Jolt path — the test asserts the *orderings*, not the absolutes):

| Profile | Draft empty → full | Sustained top speed | Turn rate | Planing |
| --- | --- | --- | --- | --- |
| Dinghy | 0.059 → 0.113 m | 9.1 m/s | 0.283 rad/s | early |
| Speedboat | 0.134 → 0.287 m | 16.0 m/s (laden 11.8) | 0.512 rad/s | empty wetted 0.59 (up on plane), laden 1.00 (plows) |
| Jetski | 0.159 → 0.214 m | 8.5 m/s¹ | 0.495 rad/s | almost immediately |
| Fishing | 0.274 → 0.481 m | 4.0 m/s | 0.076 (laden 0.060) | barely |
| Barge | 0.303 → 1.083 m | 3.8 m/s | 0.056 rad/s | never (0.00) |

¹ The jetski's top speed is limited by its light hull porpoising at the ragged
edge of the plane (its "easily flipped" character); its class is the agility and
launch, not outright speed.

Further proven: the **empty-draft ordering** dinghy < speedboat < fishing < barge
holds and every profile's **laden draft exceeds its empty draft** by a material
margin (the barge by 0.78 m — tens of cm). Under an identical knock-down the
**laden dinghy capsizes** (uprightness −1.0, stays inverted) while the **fishing
boat self-rights** (1.0); at the structural overload the fishing boat still rights
but **~2.8× slower** (2.65 s vs 0.95 s to recover). An **overloaded barge's deck
sits near awash** — freeboard 1.10 m empty → 0.12 m at `1.25×` — while its draft
goes 0.30 → 1.27 m. All five profiles and every load state are NaN-free over a
minute of Gerstner chop, including a mid-run `SetCargo` load transfer.

### Live in the demo

While the **boat** is the active vehicle, number keys **1–5** swap the boat-type
profile (dinghy/speedboat/jetski/fishing/barge) — the boat respawns on the lake
at its current pose and the audio voice swaps to the profile's engine — and key
**L** cycles the cargo load `0% → 50% → 100% → 125%` (structural overload) via
`SetCargo`, so the hull **visibly settles deeper** each step (GPU-verified: an
empty barge floats high at 0.30 m draft / 1.10 m freeboard, a `125%`-laden barge
sits buried to the deck rail at 1.27 m / 0.13 m). The HUD shows the profile name,
cargo/capacity, draft and freeboard. `RX_DRIVE_BOAT=dinghy|speed|jetski|fishing|
barge` and `RX_DRIVE_CARGO=<0..1.25>` force the initial profile and load for
headless captures.

---

## Aircraft

`rx::physics::Aircraft` — a force-based fixed-wing plane over one dynamic
fuselage body. Each step integrates a strip-theory aero model, a prop or jet,
and three-wheel landing gear. Air density is a constant 1.225 kg/m³ (ISA sea
level). Defaults describe a Cessna-172-class light single. `AircraftInput`:
`throttle` `0..1`, `pitch` / `roll` / `yaw` `-1..1` (command axes, not
deflection angles), `flaps` `0..1` (quantized to `flap_steps`), `brakes` `0..1`.
Positive `yaw` yaws the nose **right** — both the rudder in the air and the
nose-wheel steering on the ground (right is body `-X` in this engine).

### Aero model (`AircraftDesc`)

- **Wing** split into two equal halves (independent stall, so a wing drop is
  possible): lift-curve slope `wing_cl_alpha` to `wing_stall_alpha_rad`, then a
  `post_stall_decay` blend into the flat-plate curve. Induced drag
  `CL²/(π·AR·oswald_efficiency)`, parasitic `cd0`. `flap_delta_cl/cd` add lift
  and drag.
- **Ailerons** — `aileron_authority` differential ΔCL between the halves.
- **Horizontal tail / elevator** — `tail_area_m2` at `tail_arm_m` aft,
  `elevator_authority` ΔCL at full stick.
- **Vertical fin / rudder** — side force vs sideslip (`fin_cl_beta`) plus
  `rudder_authority`; `fuselage_side_cd/area` weathervane damping.
- **Inertia tensor** — the constructor overrides Jolt's collision-box inertia
  (the slim fuselage box excludes the wings, so its roll inertia is several
  times too small) with an honest tensor from the airframe geometry, using
  published single-engine-GA radii of gyration: `I_roll = m·(0.22·b/2)²`,
  `I_pitch = m·(0.34·L/2)²` (`L ≈ 1.8·tail_arm`), `I_yaw ≈ 0.85·(I_roll +
  I_pitch)`. So roll/pitch/yaw response is real, not box-derived.
- **Rotational aero damping** (`roll_damp` / `pitch_damp` / `yaw_damp`) — a
  light safety net over the strip-theory surface damping that keeps post-stall
  tumbling bounded. Now that the inertia is honest, `roll_damp` is reduced (the
  ~6× larger roll inertia plus the wings' own roll damping carry it); `pitch` /
  `yaw` stay as a light net over the strong tail/fin.
- **Propulsion** — prop: momentum-theory thrust falling off with airspeed,
  `T ≈ min(power·eff / max(V, v_min), static_cap)`, `rpm` spool-lagged. Jet:
  static thrust scaled by throttle through a spool lag, telemetry `rpm` = N1 %.

### Mass / MTOM semantics

`empty_mass_kg` + `payload_kg` (fixed at creation). Payload is clamped so
`empty + payload ≤ structural_mass_limit_kg` (a hard cap slightly above MTOM). A
plane loaded between `max_takeoff_mass_kg` (MTOM) and that limit **is created and
flies, but flies like a pig** — long ground roll, weak or no climb — purely from
the induced-drag/weight coupling in the model, not a scripted penalty.
`over_mtom()` reports the between-MTOM-and-limit state.

### Landing gear

Three legs (`0` nose/steerable, `1` left main, `2` right main). Each is a
downward suspension **raycast** from `local_pos` plus a wheel: spring/damper
along the contact normal over `travel`, `brake_force` on braked wheels,
`rolling_resistance`, and a `lateral_grip` (µ) cap on side force.
`nose_steer_angle_rad` is full low-speed nose-wheel deflection. `local_pos` is
the **real hardpoint** at/inside the fuselage box (belly `y = −0.5`); the
suspension ray casts with **self-exclusion** (`Raycast(..., body)`) so it
clears the plane's own underside instead of the old below-the-box workaround.

### Telemetry — `AircraftState`

`airspeed_mps`, `vertical_speed_mps`, `alpha_deg`, `beta_deg`, `stalled_left` /
`stalled_right`, `rpm` (prop rpm or N1 %), `engine_load` (`0..1`), `throttle`,
`on_ground`, `gear_compression[3]` (`0` extended .. `1` bottomed),
`total_mass_kg`, `over_mtom`, `position`, `rotation`. Airspeed and every aero
force are taken **relative to the airmass** — the world's global `wind()` plus
the aircraft's own `set_wind(Vec3)` local bias added on top (both default to
still air) — so a headwind shortens the ground roll and a crosswind weathervanes
the nose.

---

## Kite

![kite on the beach post](media/vehicles/drive_kite.jpg)


`rx::physics::Kite` (`engine/physics/kite.{h,cc}`) — a force-based **tethered**
kite simulator over one light dynamic sail body, layered on the same public
`PhysicsWorld` primitives (`AddForceAtPoint` / `AddTorque` / `GetPointVelocity`)
as the boat and aircraft. Unlike the boat it is **heavier-than-air**: the sail
gets ordinary Jolt gravity with no buoyancy exemption, flying purely on
aerodynamic lift. One `Kite` owns one sail body; the game moves an **anchor**
world point each frame — a hand, a fixed post, or a towing vehicle — and drives
the kite with a two-axis input (steer + reel), reading telemetry back for
HUD/camera. Sail frame: it lies in the body X-Y plane, span along body X, nose
toward +Y, tail toward −Y, belly normal along +Z. `KiteInput` is `steer`
(`-1..1`) and `reel` (`-1..1`, `+` out / `−` in).

### Aero model (`KiteDesc`)

Flat-plate **normal-force** decomposition, chosen because it stays robust at
every incidence — a kite trims at high alpha and has to survive tumbling and
violent gusts without a lift/drag-direction singularity. The relative wind
`w = wind − aero-centre point velocity` is split into a normal component
(`wn = w·n`) and a tangential one; the pressure force is
`F_n = 0.5·rho·A·cn·(w·n)·|w|` along the belly normal, so the effective
coefficient `CN = cn·sin(alpha)` grows with incidence. Its vertical part is
lift and its downwind part is drag — one inclined pressure force giving both,
exactly like a sail (`normal_coeff` = `cn`). A small `tangential_coeff`
skin/edge drag and a **tail** — a bluff `tail_drag` patch on a long `tail_length_m`
lever down −Y — add the rest; the tail weathervanes the nose into the wind and
damps yaw/roll/pitch oscillation.

- **Attitude trim (why it flies belly-to-wind at high, not zero, alpha)** — a
  real kite's bridle and camber hold the sail at a fixed high angle of attack
  belly-*into*-the-wind instead of feathering or flipping. Modelled as a
  restoring torque that aligns the belly normal (`+Z`) to a target computed each
  step from a **low-pass of the apparent wind** (`wind_ref`): the direction that
  sits at `trim_alpha_rad` (~23°) incidence belly-up — an unambiguous,
  non-flippable target (a symmetric pitch-only trim has a second belly-down
  equilibrium the tether could knock it into). The torque scales with
  `attitude_stiffness · q_dyn`, so it is firm in wind and fades to nothing in
  dead air (the sail goes limp and falls). It leaves rotation about the normal
  free — that is what steering and the tail act on. Aiming the target off the
  *mean* airmass rather than the instantaneous wind keeps the sail from chasing
  its own fast motion and flipping during a launch.
- **Bridle + pendulum** — `bridle_point` (the tether attach) is kept close to,
  slightly below, the CoM: the attitude trim owns stability, and a long bridle
  lever would let a tension spike (a taut-line snap, a fast tow) torque the sail
  hard enough to flip it. `linear_damping` (whole-sail form drag on the body
  velocity) bleeds the tangential swing as the kite arcs from launch up to its
  equilibrium — zero at a stationary equilibrium, so it costs nothing in steady
  flight and only kills the transient whip.

### Tether

A stiff **one-sided** spring (`tether_stiffness`, N/m — a string only pulls,
never pushes) from the bridle point to the anchor, damped along the line
(`tether_damping`, near-critical for the tiny sail mass so a taut line does not
ring at 60 Hz). The rest length is reeled within `[min_line_m, max_line_m]`
(`6..60` m, default `25`) at `reel_rate` (`2.5` m/s — modest, so the sail can
physically follow the shortening line). Tension is **hard-capped** at
`tether_max_tension` (`1500` N, sized well above the few-tens-of-N of steady
flight): a one-sided spring against a fast-moving or towed anchor would
otherwise inject an explosive impulse — this is the documented blow-up guard
that keeps the kitesurf/tow case bounded and NaN-free.

### Two-line steering

`steer` models the stunt-kite line-warp as a moment about the **line-of-sight**
(anchor→kite) axis, banking the sail so its lift vector carves a turn. It is
scaled by dynamic pressure (`steer_authority` reads as an effective lever arm,
m), so control authority vanishes as the wind dies — emergent loops and dives
under steer in a good breeze, a limp fall in dead air.

### Update-ordering & telemetry — `KiteState`

Like the other force simulators, `Kite::Update(input, dt)` only stages this
step's forces and **must** run before `PhysicsWorld::Update(dt)`, with
`set_anchor()` already pointing at this step's anchor (call it every frame the
anchor moves). Telemetry is refreshed from the pose sampled at the start of the
step: `position`, `rotation`, `altitude_m` (sail height above the anchor),
`tension_n` (`0` when slack), `taut`, `airspeed_mps` (`|relative wind|`),
`alpha_deg` (incidence to the relative wind) and `line_length_m` (the current
reel-adjustable rest length).

### Measured behaviour

From `test/kite_test.cc` (headless, ~1.5 m sport-kite defaults, flat ground,
steady `set_wind`):

| Scenario | Result |
| --- | --- |
| 8 m/s wind, 25 m line | Launches off the ground and climbs to a stable **overhead** equilibrium: altitude ~17–19 m (elevation ~50°), alpha ~26°, tension ~27–32 N, airspeed ~8 m/s, line taut. No oscillation — settles within ~15–20 s and holds a tight `[17.4, 19.1]` m band over 30 s. |
| Wind → 0 | Sinks (form-drag damped) and settles on the ground (~0 m). |
| Steer left vs right | ±4.1 m lateral displacement in 2 s, in opposite directions. |
| Reel in (25 → 12.5 m line) | Altitude follows 17.4 → 11.5 m; the kite stays aloft. |
| Anchor towed at 8 m/s in dead calm | Flies on apparent wind: altitude ~14.8 m, tension ~37 N. |
| 60 s violent gusting + a jerked anchor | NaN-free throughout; tension never exceeds the 1500 N cap. |

### Live in the demo

**Tab** cycles to the kite as the 4th vehicle ("KITE" in the HUD). The demo
sets a default ~7 m/s wind so it launches on its own. **A / D** steer, **W / S**
reel in / out. `RX_DRIVE_VEHICLE=kite` picks it for headless captures.

---

## Audio

Procedural, headless-capable, NaN-free and bounded. The DSP is float, render-
thread-only; parameter hand-off is the `SynthVoice`'s job.

- **`EngineSynth`** (`engine_synth.h`) — a four-stroke (or turbine) model: a
  bank of harmonics + half-order subharmonics of the firing frequency, gains
  shaped by load, overrun burble, intake/exhaust noise, and an optional prop or
  turbine layer. Driven by a pure-data `EnginePreset` (cylinders, unevenness,
  idle/redline rpm, partials, brightness, exhaust low-pass, intake level, prop
  blades/gearing, or a turbine whine sweep).
- **`SkidSynth` / `WindSynth`** (`aux_synth.h`) — band-passed noise the slip
  ratio gates (silent on a rolling tyre), and speed-driven wind rush.
- **`SynthVoice`** (`synth_voice.h`) — the procedural `Decoder` seam: renders a
  `Synth` endlessly at the mixer rate. `SetParams` (engine thread) hands over a
  `SynthParams` snapshot lock-free via a seqlock; per-sub-block one-pole
  smoothing keeps updates click-free.
- **`VehicleAudio`** (`vehicle_audio.h`) — the one class a game needs per
  vehicle: owns the engine/skid/wind voices, and `Update(VehicleAudioState)`
  publishes parameters, places the positional voices and ducks when submerged.

### The six presets

| Preset | Character |
| --- | --- |
| `InlineFourCarPreset` | buzzy, even, wide rev range |
| `V8Preset` | cross-plane rumble, uneven-fire emphasis |
| `MotorcycleTwinPreset` | high redline, hard uneven-fire pulse |
| `InboardBoatPreset` | low rpm, water-muffled exhaust (low exhaust low-pass) |
| `SinglePropPlanePreset` | engine drone + propeller blade tone |
| `LightJetPreset` | turbine whine + exhaust roar (rpm = N1 %) |

### Telemetry mapping

Two structs bridge physics to DSP. `VehicleAudioState` is the game-facing input
to `VehicleAudio`; `SynthParams` is what actually reaches the oscillators.
Values are clamped on use, so a rough estimate is fine. Per vehicle kind:

| Source | → SynthParams / VehicleAudioState |
| --- | --- |
| Car `VehicleState` | `rpm←rpm`, `load←engine_load`, `throttle←input`, `speed←forward_speed`, `slip←max wheel longitudinal_slip` (or `wheel_slip[]` per wheel), `gear`/`is_shifting` for the flare |
| Boat `BoatState` | `rpm←rpm`, `load←engine_load`, `throttle←throttle` (signed, magnitude used), `speed←speed_mps`, `submerged←!prop_submerged` |
| Aircraft `AircraftState` | `rpm←rpm` (prop rpm or N1 %), `load←engine_load`, `throttle←throttle`, `speed←airspeed_mps`; jets may add `thrust` (roar) split from `rpm` (spool) |

#### `VehicleAudioState` contract

Fields below the original block are additive and each defaults to a value that
reproduces the previous behaviour exactly, so a caller filling only the base
fields is unchanged.

| Field | Type | Default | Meaning |
| --- | --- | --- | --- |
| `rpm` | `f32` | `0` | crank rpm (turbine preset: N1 %, `0..100`) |
| `load` | `f32` | `0` | `0..1` engine load (torque demand met) |
| `throttle` | `f32` | `0` | driver throttle, **`-1..1`** (boats run astern); the **magnitude** is used |
| `speed_mps` | `f32` | `0` | ground speed, m/s |
| `slip` | `f32` | `0` | `0..1` aggregate tyre slip (fallback when no per-wheel data) |
| `submerged` | `bool` | `false` | underwater: ducks the engine gain **and** muffles the synth (`SynthParams.muffle`) |
| `position` | `Vec3` | `{}` | world position (Y-up, m) for panning |
| `wheel_slip[4]` | `f32[4]` | `{0,0,0,0}` | optional per-wheel slip, order **FL FR RL RR** |
| `wheel_count` | `u32` | `0` | `0` = ignore `wheel_slip`, use `slip`; `≥1` = intensity from the worst wheel; `≥4` = also pan the skid to the slipping side and shade its band by axle |
| `gear` | `i32` | `INT_MIN` | current gear; `INT_MIN` = unknown → **no** shift flare ever |
| `is_shifting` | `bool` | `false` | a gear change is in progress; its rising edge (with a known `gear`) fires one brief click-free flare — upshift cut if the gear rose, downshift blip if it fell |
| `thrust` | `f32` | `-1` | turbine roar level `0..1` decoupled from spool; `<0` = derive from `rpm`/N1 as before |

`SynthParams` gains matching additive fields consumed by the synths directly:
`muffle` (`0`, `0..1` output low-pass tighten + level duck), `thrust` (`-1`
sentinel = derive), `gear_shift` (`0`; `<0` upshift cut, `>0` downshift blip,
edge-triggered and passed unsmoothed so the `EngineSynth` owns the
sample-accurate envelope) and `skid_bias` (`0`, `-1` rear .. `+1` front, shades
the `SkidSynth` band). All are smoothed by `SynthVoice` except `gear_shift`.

`test/vehicle_audio_test.cc` drives the synths and the mixer path headless;
`test/vehicle_integration_test.cc` renders all three engines straight from live
telemetry inside the physics loop.

---

## The demo (`--demo drive`)

![Milk truck sliding across the ice patch](media/vehicles/drive_car.jpg)
![Boat planing across the lake](media/vehicles/drive_boat.jpg)
![Plane airborne after an auto-throttle takeoff](media/vehicles/drive_plane.jpg)

A graybox showcase of all three simulators and the surface system in one scene.

**Scene** — a ~400 × 400 m heightfield, grass by default, with:

- a painted **asphalt road loop**;
- a **300 × 20 m runway** for the aircraft;
- an **ice patch**, and **dirt** + **sand** regions (material showcase);
- a **lake** quadrant carrying the boat on the Gerstner water field.

**Models** — pulled by `tools/get_vehicles.sh` into `assets/vehicles/`:
`CesiumMilkTruck` drives, `Cesium_Air` flies, and `ToyCar` / `CarConcept` /
`GroundVehicle` are parked as a material showcase. A graybox fallback stands in
when the assets are absent.

### Controls

| Input | Action |
| --- | --- |
| **Tab** | Cycle the active vehicle |
| **1–6 / 1–5** | Car active: swap the handling profile (sports/muscle/hatch/SUV/van/semi). Boat active: swap the boat-type profile (dinghy/speedboat/jetski/fishing/barge) |
| **L** | Boat active: cycle cargo load 0% / 50% / 100% / 125% (overload) — the hull settles deeper |
| **W / S** | Throttle / brake-reverse (car + boat) or throttle up / down (plane) |
| **A / D** | Steer (car) or rudder (boat / plane) |
| **Arrows** | Plane pitch / roll |
| **Space** | Handbrake (car) / wheel brakes (plane) |
| **M** | Toggle manual gearbox |
| **Shift / Ctrl** | Shift up / down |
| **F** | Flaps 0 / 0.5 / 1 |
| **J** | Cycle rain wetness 0 / 0.5 / 1 |
| **R** | Reset the active vehicle |
| **C** | Chase / free camera |

### Headless captures

`RX_DRIVE_VEHICLE=car|boat|plane` picks the initially active vehicle and
`RX_DRIVE_AUTO=1` holds full throttle on it (takeoff flaps + a rotate/ease
elevator schedule for the plane), so screenshot/CI runs show motion without
input. `RX_DRIVE_PROFILE=sports|muscle|hatch|suv|van|semi` forces the car's
initial handling profile; `RX_DRIVE_BOAT=dinghy|speed|jetski|fishing|barge` and
`RX_DRIVE_CARGO=<0..1.25>` force the boat's profile and cargo fraction (either one
also makes the boat the initial vehicle), so a capture can show an empty barge vs
a laden one sitting deep. Combine with `RX_HIDE_DEBUG_UI=1` and the usual
`RX_UI_SHOT` / `RX_UI_SHOT_FRAMES` to take the shots above.

---

## Known gaps / future work

- **Wind is a uniform global, not a spatial field** — `PhysicsWorld::wind()` is
  one world-space airmass velocity the aero simulators sample; there is no
  per-position gust/shear field yet. Cars deliberately ignore wind (negligible
  at this fidelity).
- **Audio telemetry wishlist** — *closed.* Per-wheel slip (skid intensity from
  the worst wheel, a lateral pan toward the slipping side and a front/rear band
  shade), gear/shift flags (a brief click-free shift flare) and a jet
  spool-vs-thrust split all reach the synths now; a dynamic output muffle also
  drives the submerged/occluded voice through the synth itself. All are additive
  and default-inert (see the `VehicleAudioState` table). Still open: clutch-slip
  fidelity (the flare is a fixed ~180 ms envelope, not clutch-modulated) and
  body-yaw-accurate skid panning (the lateral bias is applied in world X, exact
  only for an unrotated body — a caller can pre-rotate `position`).
