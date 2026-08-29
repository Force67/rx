# Baked worlds

`engine/world` is the streaming half of a large map: a cooked, immutable world
that lives in an `.rxp` archive and arrives in the ECS a cell at a time.

The design principle it is built around is a negative one. A world cell is a
coarse unit for **authoring, spatial interest, persistence, I/O packaging and
scheduling**, and for nothing else. It is deliberately *not* the unit of ECS
memory, render visibility, geometry LOD, texture residency or individual object
lifetime. Every one of those wants a different granularity, and a chunk that
tries to be all of them at once forces the worst one on everybody.

## Three layers

**The index** (`RXWORLDI`, one per world) is always resident. Bounds, zone,
which domains and tiers a cell has, and what each costs once decoded. It is the
only thing a streaming decision reads. Opening a cell payload to find out where
the cell is, or how big it is, is the failure this file exists to prevent.

**The payloads** (`RXCELLPL`, one per cell per domain per tier) are immutable
and cooked. Two shapes exist: archetype-major component columns for entities,
and packed transforms for static decoration. They are addressed by convention,
`<prefix>/<cell id in hex>.<domain>.<tier>.rxcell`, rather than by a path stored
in the index, so there is one answer to where a payload lives instead of two
that can disagree. The archive's own table of contents is authoritative for
on-disk size for the same reason.

**The overlay** is the only mutable layer: destroyed stable ids and moved
transforms, sparse, keyed by stable id. A player who breaks one fence must not
cause the cell that fence was in to be rewritten. Deltas are applied *while* a
cell materializes - a destroyed row is skipped during the column copy, so its
bytes are never touched and it never briefly exists. It carries the bake id it
was recorded against, and `SetOverlay` refuses one from a different cook: stable
ids are assigned by cook order, so a mismatched overlay would not fail, it would
delete and move whatever rows now happen to carry those ids.

## Domains, not cell states

A cell does not have one residency state. Each domain has its own, with its own
radii and its own frame budget:

| Domain | Content |
|---|---|
| `kGameplay` | ECS entities that need behavior |
| `kRepresentation` | static instance pages: decoration with no ECS identity |
| `kCollision` | static collision tiles |
| `kNavigation` | navigation tiles |
| `kLighting` | light and fog lists |
| `kAudio` | emitters and portal data |

The same observer therefore produces a different bubble per domain in the same
frame, and a headless server can carry gameplay, collision and navigation at
full radius while never reading a light list. Only the first two have a payload
shape so far; the rest are named because their residency is genuinely
independent and the streamer already schedules them separately.

Geometry and texture detail are absent on purpose. Those are resource-page
residency, owned by the virtual geometry and texture streamers, and binding them
to a world cell is exactly the over-wide chunk this design rejects.

Within a domain, `Tier` picks *which* payload: `kProxy` is an HLOD or a coarse
navigation graph, `kFull` everything the cook produced. Tiers are alternatives,
not increments; exactly one is resident. The resolved tier is part of the
region's identity, so a cell crossing a distance band is an ordinary budgeted
reload the planner schedules inside the retain radius, with a hysteresis margin
so a cell sitting on the boundary does not flap. A world baked at one tier
resolves both bands to the same payload and never reloads at all.

The swap is a reload, not a crossfade: the old tier's rows are destroyed before
the new tier's arrive. The retain radius guarantees the reload happens; it does
not close the gap. For a domain carrying behavior that gap is an entity
despawning and respawning as the player walks up, which is why
`DefaultWorldStreamPolicy` leaves gameplay, collision and navigation on one tier
and bands only the domains whose gap is a moment of coarser scenery.

## The loop

`rx::scene::WorldStreamPlan` (upstream of this module) owns the lifecycle:
prepare, commit, cancel, unload, with tickets, generations and frame budgets.
`WorldStreamer` runs one plan per domain and does the work each action asks for.

```
Update(observers)
  DrainLoader              results from finished reads, generation checked
  per domain:
    gather                 EvaluateWorldStreamDemand over the index's cells
    claims                 each honored claim as a source pinned to its cell
    merge                  one candidate per cell: nearest distance, resolved
                           tier folded into the region, claim priority applied
    retire                 budgeted teardown of cells already retiring
    AdvanceWorldStreaming  the planner decides
    act                    prepare -> loader; commit -> materialize a quantum;
                           cancel/unload -> start a budgeted teardown
```

Loading is behind the `CellLoader` interface rather than a direct call into the
Vfs. That is what makes the hard parts testable: a test hands a payload back
after its generation was cancelled, or out of order, and the streamer has to
drop it. A request is identified by the whole `CellLoadRequest`, never by its
ticket alone - each domain runs its own plan and each numbers generations from
one, so a cell's first gameplay request and its first representation request
carry the same ticket.

Materialization goes through `ecs::World::CreateBatch`: the whole run appended
into one archetype, then filled column by column. `Add<T>` would walk each
entity through one intermediate archetype per component to arrive somewhere the
cook already knew.

Commit and teardown are both budgeted in rows per tick. Staying inside a memory
budget and still hitching because a cell was published in one frame is the
failure mode budgets exist to prevent, and it applies just as much to
destroying 100,000 entities as to creating them. The two are not symmetric: the
planner admits at most `maximum_commit_steps` cells per tick, while every
retiring cell gets a teardown quantum, so a tick can destroy more rows than it
creates. `maximum_pending` is what bounds that.

A payload that fails to load is retried a few times and then throttled: a cook
error is deterministic, so the alternative is re-reading the same broken bytes
every retry interval forever. It is a throttle rather than a ban because the
streamer cannot tell a broken cook from an archive that went missing for a
second, so a suppressed cell is offered again long after and heals if it can. A
host that has just changed what is mounted can say so with `ClearFailures`, and
`stats().suppressed` counts the cells that are quietly gone.

## Identity across a boundary

An `ecs::Entity` handle is reused with a new generation as soon as its slot is
freed, so nothing outside a resident cell may hold one. Everything that refers
across a streaming boundary - a save file, a quest, a network peer - uses the
stable id, and resolves it through `WorldStreamer::Resolve`. Stable-id ranges
never overlap between cells, so an id resolves to its owning cell by binary
search with nothing resident.

Component columns are named, never numbered: `ecs::ComponentId` is assigned in
first-use order at runtime and differs between runs, so it can never reach disk.
Each column carries the cook's hash of the component's reflected field layout,
which the loader recomputes and compares. Column bytes are the struct verbatim,
so an ABI drift that went unnoticed would corrupt every entity in the cell
instead of failing.

Three things are refused rather than copied: a component that needs a
constructor, one whose reflected layout no longer matches the bake, and one with
an entity-reference field - a handle is reused with a new generation as soon as
its slot is freed, so a restored one names whatever unrelated entity now sits
there. What cannot be caught is a member nobody reflected: neither the layout
hash nor the field check can see one, so a component meant to be baked has to be
fully reflected, and that is a rule for whoever writes it rather than something
the loader can enforce.

**A stable id is a streaming key, not yet a persistence key.** It is assigned by
cook order within a cell, so re-baking a changed scene, or moving one object
across a cell boundary, can reassign it. The bake id stamped into the index,
every payload and every serialized overlay catches the mismatch loudly, but a
save that must survive re-cooking needs an authored identity the cook maps to an
id, which nothing here provides yet.

## Static decoration

A noninteractive rock needs to be drawn and stood on. It does not need an ECS
identity. Instance pages hold a prototype, a transform and a stable world id,
and cost no row. `Instances(cell)` gives the rows and `Prototypes(cell)` the
names their `prototype` field indexes; the names outlive the payload they were
decoded from, because that payload is dropped the moment the cell publishes.

`WorldStreamer::Promote` turns one into a real entity when something finally
needs its behavior - damage, a script, a player - with the same stable id it
always had. The page row stays and is marked `promoted`: nothing removes it, so
a renderer walking the page skips those or draws the same rock twice.

A promoted entity belongs to its cell, so it does not survive that cell being
unloaded or reloaded, and neither does anything the game attached to it. That
includes a tier change, which is why the default policy does not band the
domains a game is likely to promote from. A game that needs promotions to
persist records them in its own save data against the stable id and re-promotes
after a load.

## Residency claims

A pin says "keep this loaded" and, six months later, nobody can say who said it
or when it stops being true. A `ResidencyClaim` carries an owner, a reason, a
kind and a deadline, and `ClaimSet::Explain` answers "why is this cell
resident?" directly.

A claim is not a second mechanism beside streaming. An honored claim becomes a
streaming source standing at the middle of one cell with no radius - which is
what a teleport destination or a running quest always was - so the planner
weighs it exactly as it weighs a player. Hard claims outrank ordinary demand and
are never revocable; weaker ones stop counting when the host raises the bar
under memory pressure.

A claim admits a cell; it does not decide its detail. The tier band follows the
real observers, so taking or dropping a lease never evicts and rebuilds a cell
that was already resident and correct - which matters because a lease that is
re-issued periodically would otherwise put its cell on a permanent unload and
reload treadmill. A claim that does want the near tier says so explicitly.

The set has no clock: `Expire` is the host's to call. A lease nobody expires is
the immortal pin it exists to replace.

## Cooking

```sh
rxworld bake city.rxscene city.rxp --name city --cell-size 64
rxworld inspect city.rxp --name city
```

`bake` sorts every entity into a grid cell by world position, groups each cell's
entities by component set, and writes one archetype-major payload per cell and
domain, with the index beside them in one archive. Entities that are only a
transform and a mesh become instance page rows.

Two things it refuses rather than guesses: a `Parent` link, because an ECS
handle cannot survive a bake, and a component this build does not register
(`--skip-unknown` to override). A third it neither refuses nor bakes: a
component holding an indirection (`Name`, anything with a `std::string`) cannot
be restored by copying bytes, so it is dropped with a warning naming it. An
authored world that needs one needs a different representation for it.

The bake id is a hash of the scene, the cook settings and the schema the cooking
build can bake, so an unchanged rebuild produces the same id, and a changed
scene, a different cell size or a build that disagrees about a component's
fields all produce a different one that an old index refuses to read.

`rxworld` reflects only the components its own build registers. A game cooks
from a build that registers its own.

## Verifying it

Five test binaries, all in plain `ctest`, no GPU (the last needs
`RX_BUILD_TOOLS`, since it drives the tool):

| | |
|---|---|
| `world_format_test` | round trip, the cook-time consistency checks, and the decoders' in-body structural checks driven by crafted bytes with the checksum repaired |
| `world_map_test` | an index out of a real `.rxp`, per-domain bubbles, and the refusals for a stale, holed or out-of-range archive |
| `world_overlay_test` | the delta semantics and the invariants a decoded save has to hold |
| `world_stream_test` | cancellation races, stale generations, budgeted commit and teardown, tier bands, claims, overlays, promotion, multi-archetype and chunk-spanning cells |
| `world_bake_test` | the whole path: an authored scene through the real `rxworld` binary into an archive, mounted and streamed back |

The streaming tests drive a hand-controlled `CellLoader`, so completion order,
late results after a cancel, and transient read failures are all things a test
decides rather than things it waits for.

## What is not here

Named, so nobody has to discover them:

- No deadline scheduler. Priority is distance, urgency, starvation age and a
  claim's rank; there is no benefit/cost model and no per-resource budget beyond
  rows per tick and the planner's counts.
- No memory-pressure signal, so no automatic degradation ladder. The host raises
  `ClaimSet`'s bar itself.
- No teleport barrier. The pieces are there (claim the destination, wait for it
  to become resident) but the minimum-viable-arrival API is not.
- No spatial index. The candidate gather walks every cell; a broad phase belongs
  in front of it once a world is large enough to want one, with the same exact
  test on its survivors.
- No cross-cell references, and no zones or portals beyond the `zone` field the
  index carries. The baker refuses a hierarchy rather than pretend.
- The overlay records deletions and moves. It cannot express a spawned entity or
  one that moved between cells.
- No re-promotion hook: nothing tells a game that a cell it promoted from is
  about to retire, and a promoted entity does not survive that.
- Nothing outside the tests and `rxworld` links `rx::world` yet: no host drives
  it, so the numbers in `DefaultWorldStreamPolicy` are a starting point rather
  than a measurement.
