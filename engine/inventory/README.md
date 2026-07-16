# rx::inventory

A **batteries-included but strictly optional** ECS inventory system for the rx
engine: item catalogs, stacking inventories, generic equipment slots, dropping
items into the 3D world as physics objects, and a performant "dropped items lie
there forever" persistence story.

## Philosophy

- **Optional.** Games that use rx are never forced to use this. It is a
  self-contained module with zero coupling into other modules beyond the public
  `ecs` / `scene` / `physics` APIs. Nothing else in the engine depends on it.
- **No game taxonomy.** The engine defines *mechanics* (stacking, weight,
  capacity, equip, drop, persist), never *semantics*. There are no
  `sword`/`potion` enums. An `ItemDef` carries a `name_hash`, a `flags` bitset
  and a `payload`, all interpreted solely by the game. Equipment slots are
  identified by a game-hashed `u32` tag (`"hand.right"`, `"head"`, ...).
- **Functional-first.** Plain-data components + free-function systems, matching
  `scene/camera_rig.h`. State lives in components; behaviour lives in free
  functions you call. The `ItemCatalog` is passed explicitly — there is no
  global catalog.
- **Two targets.** `rx::inventory` is physics-free (catalog, inventories,
  equipment, their persistence) so a dedicated server or a drop-free game links
  it without Jolt. `rx::inventory_world` adds world drops and pulls `rx::physics`
  (the same split as `rx::nav` / `rx::nav_viz`).

## Data model

| Type | Kind | Purpose |
|------|------|---------|
| `ItemDefId` (`u32`) | id | Game-assigned; `0` = none. Persisted key. |
| `ItemDef` | catalog data | name hash, world mesh, physics shape + mass/friction/restitution + scale, `max_stack`, `weight`, `flags`, `payload`. |
| `ItemCatalog` | game-owned table | `Register` / `Find`; passed explicitly to systems. |
| `InventoryEntry` | stack | `(item, count, payload)`. Distinct `payload` never merges. |
| `Inventory` | component | `entries`, `max_weight`, `max_entries`, `revision` (UI dirty-check). |
| `EquipmentSlot` | slot | `(tag, item, payload, occupied)`. References the inventory; stores no count. |
| `Equipment` | component | `slots`, `revision`. Inventory stays the source of truth. |
| `WorldItem` | component | dropped item: `item`, `count`, `payload`, `body` (physics::BodyId), `persistent_id`, `at_rest`. |
| `WorldItemRecord` / `WorldItemStore` | dormant store | compact spatial-cell-keyed backing for hibernated items. |

Components are non-POD (they own `std::vector`s); the ECS relocates them through
their move constructors, which the chunked archetype storage supports.

## Operations (free functions)

Inventory (`inventory.h`, physics-free):
`InventoryCount`, `InventoryWeight`, `AddItem` (stacking + capacity + weight),
`RemoveItem`, `TransferItem` (containers/looting), `FindSlot`, `Equip`,
`Unequip`, `ValidateEquipment`.

World items (`world_item.h`, physics):
`DropItem`, `PickUpItem`, `SyncWorldItems`, `HibernateDistantWorldItems`,
`WakeWorldItemsNear`, `SaveWorldItems`, `LoadWorldItems`,
`NextWorldItemId` / `ReserveWorldItemId`.

Persistence (`serialize.h`, physics-free): `SaveInventories`, `LoadInventories`.

## Per-tick wiring order

A game that drops items into the world runs, inside its simulation stage, after
`physics.Update(dt)`:

1. `SyncWorldItems(world, physics)` — copy body transforms into entity
   Transforms for **awake** items; latch settled ones to `at_rest`.
2. `HibernateDistantWorldItems(world, physics, store, player_pos, hibernate_r)`
   — evict at-rest items beyond `hibernate_r` into the store (body + entity
   freed).
3. `WakeWorldItemsNear(world, physics, catalog, store, player_pos, wake_r)` —
   re-materialise stored items within `wake_r`. **Use `wake_r < hibernate_r`**
   (hysteresis) so a boundary item does not thrash.

Inventory/equipment mutations happen wherever gameplay needs them (pickups,
crafting, trade); they carry no ordering requirement. Call `ValidateEquipment`
after any bulk removal so equipped slots referencing vanished items clear.

## Persistence & hibernation model

### Format: custom versioned binary, not edit-reflection

Inventories and world items serialize to compact, explicit-little-endian blobs
(`byte_io.h`): a 4-byte magic + `u32` version + records. This was chosen over
`engine/edit/` reflection + `.rxscene` deliberately:

- Inventories are **frequently-mutated gameplay state** written to a *game save
  file*, not editor scene documents. They want a tiny, stable, self-describing
  wire format independent of the editor module's schema and independent of the
  `edit` target (which the core `rx::inventory` does not — and should not — link).
- The blob is versioned so it can evolve without a reflection dependency.

Inventories are keyed by `scene::Guid` (the engine's stable, handle-independent
identity), so saved state reattaches to the right entity after ECS handle reuse.
World items are keyed by a stable `persistent_id`.

### Hibernation: eviction to a spatial store (the perf story)

The scaling cost of "thousands of dropped items lie there forever" is the
**physics body** — every live Jolt body sits in the broadphase and the active
island lists. `HibernateDistantWorldItems` therefore, for at-rest items beyond
the radius, **destroys the body, moves the item into a `WorldItemStore`, and
destroys the ECS entity**. A dormant item becomes a ~64-byte `WorldItemRecord`
in a vector bucketed by cubic spatial cell — no body, no entity, no render
iteration. `WakeWorldItemsNear` re-materialises entity + body from the record,
preserving `persistent_id`, when the player returns.

*Design choice — evict vs. park.* We evict (destroy the entity) rather than park
(keep the entity, drop only the body). Parking still pays ECS storage and
per-system iteration for every item ever dropped; eviction to a cell-bucketed
store makes a distant item cost only a struct, and is exactly what a streaming
world wants (query/save/load by region). The trade-off: a woken item gets a new
ECS handle — which is fine because gameplay and saves key on `persistent_id`,
not the handle. Items settle first (`at_rest`) before they are eligible, so an
item still bouncing is never evicted mid-flight.

## Physics API gaps worked around

The consumed `physics::PhysicsWorld` exposes no **sleep-state query** and no
**velocity readback**. So `SyncWorldItems` approximates "awake" with a per-item
motion check: it compares the body transform against the previous sync and, once
the item has been near-motionless for a few consecutive syncs, latches
`WorldItem::at_rest` and stops polling it (near-zero steady-state cost). A
sleeping Jolt body reports an unchanged transform, so this reliably latches once
the item settles. Consequence: if an already-at-rest item is later disturbed by
an external force, the module will not notice on its own — the game should clear
that item's `at_rest` flag when it applies such a force. A future
`IsBodyActive(BodyId)` (or velocity getter) on `PhysicsWorld` would let
`SyncWorldItems` use the engine's real sleep state instead of the heuristic.
