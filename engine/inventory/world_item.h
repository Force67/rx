#ifndef RX_INVENTORY_WORLD_ITEM_H_
#define RX_INVENTORY_WORLD_ITEM_H_

#include <functional>
#include <unordered_map>
#include <vector>

#include "core/math.h"
#include "core/types.h"
#include "ecs/entity.h"
#include "inventory/components.h"
#include "inventory/export.h"
#include "inventory/item_catalog.h"
#include "scene/components.h"

// Dropping items into the 3D world as physics bodies, keeping thousands of them
// "lying there forever" cheaply, and persisting them. This half depends on
// physics and lives in the rx::inventory_world target; a game with no world
// drops links only rx::inventory and never pulls Jolt.

namespace rx::ecs {
class World;
}
namespace rx::physics {
class PhysicsWorld;
}

namespace rx::inventory {

// A dormant dropped item: everything needed to re-materialise a WorldItem
// entity + body, with zero ECS or physics cost while it sleeps.
struct WorldItemRecord {
  u64 persistent_id = 0;
  ItemDefId item = kInvalidItemDef;
  u32 count = 1;
  u64 payload = 0;
  scene::Transform transform;
};

// Compact spatial store for hibernated world items. Records are bucketed into
// cubic cells of `cell_size` metres; TakeNear pulls back every record whose
// cell overlaps a sphere. This is the backing store for the "thousands of items
// lie there forever" story: a dormant item is a ~64-byte record in a vector,
// not an ECS entity and not a Jolt body (the only per-item cost that scales
// badly through the broadphase and active-island lists).
class RX_INVENTORY_WORLD_EXPORT WorldItemStore {
 public:
  void set_cell_size(f32 metres) { cell_size_ = metres > 0 ? metres : cell_size_; }
  f32 cell_size() const { return cell_size_; }

  void Insert(const WorldItemRecord& record);
  // Removes and returns every record whose cell overlaps the query sphere. The
  // cell granularity means a few records just outside `radius` may come back;
  // that is harmless (they re-hibernate next sweep).
  std::vector<WorldItemRecord> TakeNear(const Vec3& center, f32 radius);
  // Visits every record (for serialization). The callback must not mutate.
  void ForEach(const std::function<void(const WorldItemRecord&)>& fn) const;
  size_t size() const { return count_; }
  void Clear();

 private:
  struct CellKey {
    i32 x = 0, y = 0, z = 0;
    bool operator==(const CellKey&) const = default;
  };
  struct CellKeyHash {
    size_t operator()(const CellKey& key) const;
  };
  CellKey CellOf(const Vec3& p) const;

  std::unordered_map<CellKey, std::vector<WorldItemRecord>, CellKeyHash> cells_;
  f32 cell_size_ = 32.0f;
  size_t count_ = 0;
};

// --- drop / pick up ---

// Drops `count` units of `source`'s Inventory entry `entry_index` into the
// world as a dynamic physics body at `spawn`, applying `impulse` (kg*m/s) so it
// can be thrown or scattered. The dropped units are removed from the source
// inventory. Returns the new world-item entity, or kInvalidEntity when the
// source lacks that entry, the def is unknown, or count is 0. A scene::Renderable
// is attached iff the ItemDef carries a world_mesh; otherwise the caller is
// free to attach its own visuals to the returned entity.
RX_INVENTORY_WORLD_EXPORT ecs::Entity DropItem(ecs::World& world, physics::PhysicsWorld& physics,
                                               const ItemCatalog& catalog, ecs::Entity source,
                                               u32 entry_index, u32 count,
                                               const scene::Transform& spawn, const Vec3& impulse);

// Picks a world item up into `receiver`'s Inventory. On a full pickup the body
// is removed and the entity destroyed; on a partial pickup (receiver capacity)
// the world item's count is reduced and it stays in the world. Returns true iff
// fully picked up. `receiver` must have an Inventory. `catalog` is consulted for
// stacking/weight limits while adding.
RX_INVENTORY_WORLD_EXPORT bool PickUpItem(ecs::World& world, physics::PhysicsWorld& physics,
                                          const ItemCatalog& catalog, ecs::Entity item_entity,
                                          ecs::Entity receiver);

// --- per-tick maintenance ---

// Copies body transforms into entity Transforms for AWAKE world items only.
// Once an item stays near-motionless for a few calls it is flagged at_rest and
// skipped thereafter, so a field of settled loot costs almost nothing per tick.
// Because the physics API exposes no sleep-state query, "awake" is approximated
// by a per-item motion/still-frame count (see the README "physics gaps"). A
// game that re-disturbs a settled item should clear its WorldItem::at_rest flag.
RX_INVENTORY_WORLD_EXPORT void SyncWorldItems(ecs::World& world, physics::PhysicsWorld& physics);

// Evicts at-rest world items farther than `radius` from `center`: destroys the
// physics body, moves the item into `store`, and destroys the entity. Awake
// items are left alone (still settling). Run this after SyncWorldItems.
RX_INVENTORY_WORLD_EXPORT void HibernateDistantWorldItems(ecs::World& world,
                                                          physics::PhysicsWorld& physics,
                                                          WorldItemStore& store,
                                                          const Vec3& center, f32 radius);

// Re-materialises stored items within `radius` of `center`: recreates the
// entity (Transform + WorldItem [+ Renderable]) and a dynamic body from the
// ItemDef, preserving persistent_id. Newly woken items start awake so
// SyncWorldItems re-settles them. Use a SMALLER wake radius than the hibernate
// radius so an item near the boundary does not thrash between the two states.
RX_INVENTORY_WORLD_EXPORT void WakeWorldItemsNear(ecs::World& world, physics::PhysicsWorld& physics,
                                                  const ItemCatalog& catalog, WorldItemStore& store,
                                                  const Vec3& center, f32 radius);

// Persistent-id minting is the one piece of process-stateful module state
// (fresh ids for freshly dropped items). LoadWorldItems calls ReserveWorldItemId
// for every id it reads so later drops never collide with loaded ones.
RX_INVENTORY_WORLD_EXPORT u64 NextWorldItemId();
RX_INVENTORY_WORLD_EXPORT void ReserveWorldItemId(u64 seen_id);

// --- persistence (world items, incl. hibernated) ---

// Serializes every live WorldItem entity AND every record in `store` into one
// versioned little-endian blob; live/dormant status is preserved.
RX_INVENTORY_WORLD_EXPORT std::vector<u8> SaveWorldItems(ecs::World& world,
                                                         const WorldItemStore& store);
// Restores a SaveWorldItems blob: live records become entities + bodies,
// dormant records go into `store`. Returns false on a corrupt/unsupported blob.
RX_INVENTORY_WORLD_EXPORT bool LoadWorldItems(ecs::World& world, physics::PhysicsWorld& physics,
                                              const ItemCatalog& catalog, WorldItemStore& store,
                                              const std::vector<u8>& blob);

}  // namespace rx::inventory

#endif  // RX_INVENTORY_WORLD_ITEM_H_
