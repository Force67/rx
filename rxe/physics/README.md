# Physics

`rx::physics` keeps Jolt types behind `PhysicsWorld` so games can configure and
step one rigid/soft-body world without depending on Jolt headers. Cloth creation
is data-oriented through `ClothDesc`; the caller owns descriptor arrays only for
the duration of `CreateCloth`.

## Cloth

The cloth path accepts any welded, consistently wound manifold triangle mesh,
with or without a boundary. It is not specialized to a rectangular grid:
curtains, capes, cylindrical skirts and closed inflatables use the same API.

Jolt 5.6 supplies the parallel XPBD update, rigid-body contacts, anisotropic
edge constraints, shear, distance or dihedral bending, geodesic long-range
attachments, skeletal skin constraints/backstops, damping and pressure. rx adds
the missing cloth self-collision pass. It builds a swept-AABB BVH, excludes
one-ring topology, conservatively advances vertex/triangle and edge/edge pairs,
and writes only bounded velocity corrections before Jolt's update. Broad phase
remains `O(n log n)` without voxel blowups on irregular or fast-moving cages,
and Jolt's internal solver stays untouched.

`tools/get_jolt.sh` requires an exact, clean `v5.6.0` checkout. Keep local Jolt
patches in a separate checkout; the helper rejects modified or differently
tagged sources instead of silently producing a non-reproducible engine build.

Use a coarse, well-shaped simulation cage and bind the detailed render mesh to
its triangles offline. This is both faster and more stable than simulating UV
seams and every render vertex. Material UVs on the cage orient warp and weft;
`ClothDesc::uvs` is optional when isotropic behavior is sufficient.

Attachments are explicit:

- `pins` plus `SetClothTransform` or `SetClothPinTargets` suit curtains, flags
  and hooks.
- `ClothSkinConstraint` plus `SetClothJointTransforms` uses Jolt's native
  animated skin targets and backstop spheres for clothing.
- Kinematic capsules/convex bodies remain the character collision proxy, so
  cloth and rigid animation share the normal Jolt collision path.

`SetClothWind` takes air velocity, not a constant acceleration. Force is
computed per triangle from area, orientation and relative velocity. Pressure is
Jolt's `n*R*T` coefficient and is accepted only for one outward-wound closed
shell; disconnected or reflected volumes are rejected.

The `physics.cloth` runtime feature is enabled by default and can be disabled
with `RX_FEATURES=-physics.cloth`. `RX_JOLT=OFF` keeps the API linkable and
returns invalid handles.

## Performance

- Call `SoftBodySharedSettings::Optimize()` once at creation; Jolt then batches
  independent constraints across its worker pool.
- Prefer distance bending for incidental cloth and dihedral bending for curved
  garment rest poses.
- Keep self-collision distance below typical edge length and use one or two
  self-collision iterations unless a hero garment visibly needs more. Continuous
  contacts preserve layer side under fast crossings; correction speed remains
  bounded by `max_linear_velocity`.
- Sleeping cloth skips wind and self-collision work. Changed wind, pins,
  skinning or pressure activate it.
- A 32 MiB Jolt stack arena serves normal updates; unusual cloth peaks use a
  malloc fallback rather than terminating the process.

Jolt does not collide separate soft bodies with each other. Self-collision is
within each cloth instance; layered garments should therefore be authored as
one simulation mesh when inter-layer contact is required.

The SIGGRAPH Asia course [Recent Advances in Realistic Cloth
Rendering](https://doi.org/10.1145/3680532.3689587) informs the split between a
coarse deformation cage and a filtered, fiber-aware render surface. It is a
rendering survey, not the simulation algorithm: simulation remains Jolt XPBD
plus the collision extension described above.
