// Acceptance test for rx::inventory: stacking/capacity/weight, transfer,
// equip/unequip, and the world-item lifecycle on real Jolt (drop -> settle ->
// pick up / hibernate / wake) plus save/load roundtrips for inventories AND
// world items (including hibernated records).
#include <cmath>
#include <cstdio>
#include <vector>

#include "ecs/world.h"
#include "inventory/inventory.h"
#include "inventory/item_catalog.h"
#include "inventory/serialize.h"
#include "inventory/world_item.h"
#include "physics/physics_world.h"
#include "scene/components.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

using namespace rx;
using namespace rx::inventory;

ItemDef MakeBoxDef(f32 mass, u32 max_stack, f32 weight) {
  ItemDef def;
  def.name_hash = 0x1234;
  def.shape.kind = physics::ShapeDesc::Kind::kBox;
  def.shape.half_extents = Vec3{0.1f, 0.1f, 0.1f};
  def.mass = mass;
  def.friction = 0.6f;
  def.restitution = 0.0f;
  def.max_stack = max_stack;
  def.weight = weight;
  return def;
}

void TestStackingAndCapacity() {
  ItemCatalog catalog;
  ItemDefId apple = catalog.Register(1, MakeBoxDef(1.0f, 10, 0.5f));
  CHECK(catalog.Find(apple) != nullptr);
  CHECK(catalog.Find(999) == nullptr);  // unknown def

  // Stacking: 25 into stacks of 10 -> 10,10,5.
  Inventory inv;
  u32 added = AddItem(inv, catalog, apple, 25);
  CHECK(added == 25);
  CHECK(InventoryCount(inv, apple) == 25);
  u32 stacks = 0;
  for (auto& e : inv.entries)
    if (e.count > 0) ++stacks;
  CHECK(stacks == 3);
  CHECK(inv.revision == 1);

  // Unknown def adds nothing.
  CHECK(AddItem(inv, catalog, 999, 5) == 0);

  // max_entries cap: 2 stacks of 10 = 20 max.
  Inventory capped;
  capped.max_entries = 2;
  CHECK(AddItem(capped, catalog, apple, 25) == 20);
  CHECK(InventoryCount(capped, apple) == 20);

  // Weight cap: weight 2 each, max_weight 10 -> 5 fit.
  ItemCatalog heavy;
  ItemDefId brick = heavy.Register(2, MakeBoxDef(1.0f, 100, 2.0f));
  Inventory wlim;
  wlim.max_weight = 10.0f;
  CHECK(AddItem(wlim, heavy, brick, 25) == 5);
  CHECK(InventoryWeight(wlim, heavy) == 10.0f);
}

void TestRemoveAndTransfer() {
  ItemCatalog catalog;
  ItemDefId coin = catalog.Register(1, MakeBoxDef(1.0f, 100, 0.0f));

  Inventory a;
  AddItem(a, catalog, coin, 30);
  CHECK(RemoveItem(a, coin, 12) == 12);
  CHECK(InventoryCount(a, coin) == 18);
  CHECK(RemoveItem(a, coin, 100) == 18);  // clamps to available
  CHECK(InventoryCount(a, coin) == 0);

  // Transfer bounded by dst capacity.
  Inventory src, dst;
  dst.max_entries = 1;  // one stack of 100
  AddItem(src, catalog, coin, 250);
  u32 moved = TransferItem(src, dst, catalog, coin, 250);
  CHECK(moved == 100);
  CHECK(InventoryCount(dst, coin) == 100);
  CHECK(InventoryCount(src, coin) == 150);  // src lost exactly what moved
}

void TestEquip() {
  ItemCatalog catalog;
  ItemDefId sword = catalog.Register(1, MakeBoxDef(1.0f, 1, 0.0f));
  const u32 kHandRight = 0xAABBCCDD;  // a game-hashed slot tag

  Inventory inv;
  Equipment eq;
  AddItem(inv, catalog, sword, 1);

  // Cannot equip what you do not have.
  CHECK(Equip(inv, eq, kHandRight, 2) == false);
  // Equip keeps inventory as the source of truth (count unchanged).
  CHECK(Equip(inv, eq, kHandRight, sword) == true);
  CHECK(InventoryCount(inv, sword) == 1);
  EquipmentSlot* slot = FindSlot(eq, kHandRight);
  CHECK(slot && slot->occupied && slot->item == sword);

  CHECK(Unequip(eq, kHandRight) == true);
  CHECK(FindSlot(eq, kHandRight)->occupied == false);
  CHECK(Unequip(eq, kHandRight) == false);  // already empty

  // ValidateEquipment clears slots whose item left the inventory.
  Equip(inv, eq, kHandRight, sword);
  RemoveItem(inv, sword, 1);
  CHECK(ValidateEquipment(inv, eq) == 1);
  CHECK(FindSlot(eq, kHandRight)->occupied == false);
}

void TestInventorySaveLoad() {
  ItemCatalog catalog;
  ItemDefId gem = catalog.Register(7, MakeBoxDef(1.0f, 10, 1.0f));
  const u32 kNeck = 0x1111;

  ecs::World a;
  ecs::Entity e0 = a.Create();
  a.Add(e0, scene::Guid{100});
  Inventory inv0;
  a.Add(e0, inv0);
  AddItem(*a.Get<Inventory>(e0), catalog, gem, 15);
  Equipment eq0;
  a.Add(e0, eq0);
  Equip(*a.Get<Inventory>(e0), *a.Get<Equipment>(e0), kNeck, gem);

  ecs::Entity e1 = a.Create();
  a.Add(e1, scene::Guid{200});
  Inventory inv1;
  a.Add(e1, inv1);
  AddItem(*a.Get<Inventory>(e1), catalog, gem, 3);

  std::vector<rx::u8> blob = SaveInventories(a);
  CHECK(!blob.empty());

  // Load into a fresh world; entities are recreated by Guid.
  ecs::World b;
  CHECK(LoadInventories(b, blob) == true);
  int seen = 0;
  b.Each<scene::Guid, Inventory>([&](ecs::Entity e, scene::Guid& g, Inventory& inv) {
    ++seen;
    if (g.value == 100) {
      CHECK(InventoryCount(inv, gem) == 15);
      Equipment* eq = b.Get<Equipment>(e);
      CHECK(eq != nullptr);
      CHECK(FindSlot(*eq, kNeck) && FindSlot(*eq, kNeck)->occupied);
    } else if (g.value == 200) {
      CHECK(InventoryCount(inv, gem) == 3);
    } else {
      CHECK(false);
    }
  });
  CHECK(seen == 2);

  // Corrupt blob is rejected.
  std::vector<rx::u8> junk = {0, 1, 2, 3};
  CHECK(LoadInventories(b, junk) == false);
}

// Steps physics and syncs world items until it settles (or the budget runs out).
void StepUntilRest(ecs::World& world, physics::PhysicsWorld& physics, ecs::Entity item,
                   int max_steps) {
  for (int i = 0; i < max_steps; ++i) {
    physics.Update(1.0f / 60.0f);
    SyncWorldItems(world, physics);
    WorldItem* wi = world.Get<WorldItem>(item);
    if (wi && wi->at_rest) break;
  }
}

void TestWorldItemLifecycle() {
  physics::PhysicsWorld physics;
  CHECK(physics.Initialize());
  // Static ground: top surface at y = 0.5.
  physics.AddStaticBox(Vec3{0, 0, 0}, Vec3{10, 0.5f, 10});

  ItemCatalog catalog;
  ItemDefId crate = catalog.Register(1, MakeBoxDef(1.0f, 5, 1.0f));

  ecs::World world;
  ecs::Entity dropper = world.Create();
  Inventory dropperInv;
  world.Add(dropper, dropperInv);
  CHECK(AddItem(*world.Get<Inventory>(dropper), catalog, crate, 3) == 3);

  // Drop 2 crates from height; they fall onto the ground box and settle.
  scene::Transform spawn;
  spawn.position[1] = 4.0f;
  ecs::Entity item = DropItem(world, physics, catalog, dropper, 0, 2, spawn, Vec3{0, 0, 0});
  CHECK(item.index != 0xffffffff);
  CHECK(InventoryCount(*world.Get<Inventory>(dropper), crate) == 1);  // 2 left the inventory
  WorldItem* wi = world.Get<WorldItem>(item);
  CHECK(wi != nullptr);
  CHECK(wi->body != 0);
  CHECK(wi->count == 2);

  StepUntilRest(world, physics, item, 400);
  wi = world.Get<WorldItem>(item);
  CHECK(wi->at_rest == true);
  scene::Transform* t = world.Get<scene::Transform>(item);
  // Rests on the ground: box half 0.1 on top of ground top 0.5 ~= 0.6.
  CHECK(t->position[1] > 0.3f && t->position[1] < 0.9f);

  // Pick it up into a receiver inventory: body + entity removed.
  ecs::Entity receiver = world.Create();
  Inventory recvInv;
  world.Add(receiver, recvInv);
  rx::u32 bodies_before = physics.dynamic_body_count();
  CHECK(bodies_before >= 1);
  CHECK(PickUpItem(world, physics, catalog, item, receiver) == true);
  CHECK(world.IsAlive(item) == false);
  CHECK(InventoryCount(*world.Get<Inventory>(receiver), crate) == 2);
  CHECK(physics.dynamic_body_count() == bodies_before - 1);  // body removed
}

void TestHibernateWakeAndWorldSave() {
  physics::PhysicsWorld physics;
  CHECK(physics.Initialize());
  physics.AddStaticBox(Vec3{0, 0, 0}, Vec3{10, 0.5f, 10});

  ItemCatalog catalog;
  ItemDefId rock = catalog.Register(1, MakeBoxDef(1.0f, 5, 1.0f));

  ecs::World world;
  ecs::Entity dropper = world.Create();
  Inventory inv;
  world.Add(dropper, inv);
  AddItem(*world.Get<Inventory>(dropper), catalog, rock, 1);

  scene::Transform spawn;
  spawn.position[1] = 3.0f;
  ecs::Entity item = DropItem(world, physics, catalog, dropper, 0, 1, spawn, Vec3{0, 0, 0});
  StepUntilRest(world, physics, item, 400);
  CHECK(world.Get<WorldItem>(item)->at_rest == true);
  rx::u64 pid = world.Get<WorldItem>(item)->persistent_id;
  f32 rest_y = world.Get<scene::Transform>(item)->position[1];

  // Player far away: hibernate. Body + entity gone, one record in the store.
  WorldItemStore store;
  HibernateDistantWorldItems(world, physics, store, Vec3{1000, 0, 0}, 50.0f);
  CHECK(store.size() == 1);
  CHECK(world.IsAlive(item) == false);
  int live = 0;
  world.Each<WorldItem>([&](ecs::Entity, WorldItem&) { ++live; });
  CHECK(live == 0);

  // Save while one item is hibernated; then wake near the origin.
  std::vector<rx::u8> blob = SaveWorldItems(world, store);

  WakeWorldItemsNear(world, physics, catalog, store, Vec3{0, 0, 0}, 40.0f);
  CHECK(store.size() == 0);
  ecs::Entity woken = ecs::kInvalidEntity;
  world.Each<WorldItem>([&](ecs::Entity e, WorldItem& wi) {
    woken = e;
    CHECK(wi.persistent_id == pid);
    CHECK(wi.body != 0);
  });
  CHECK(woken.index != 0xffffffff);
  CHECK(std::abs(world.Get<scene::Transform>(woken)->position[1] - rest_y) < 0.01f);

  // Load the saved blob (item was hibernated at save time) into a fresh world:
  // it restores as a dormant store record with the same persistent id.
  ecs::World world2;
  WorldItemStore store2;
  CHECK(LoadWorldItems(world2, physics, catalog, store2, blob) == true);
  CHECK(store2.size() == 1);
  bool found = false;
  store2.ForEach([&](const WorldItemRecord& rec) {
    if (rec.persistent_id == pid && rec.item == rock) found = true;
  });
  CHECK(found);

  // A live save/load roundtrip too: drop, keep live, save, reload -> live entity.
  ecs::World live_world;
  ecs::Entity live_dropper = live_world.Create();
  Inventory ld;
  live_world.Add(live_dropper, ld);
  AddItem(*live_world.Get<Inventory>(live_dropper), catalog, rock, 1);
  scene::Transform s2;
  s2.position[1] = 2.0f;
  ecs::Entity live_item =
      DropItem(live_world, physics, catalog, live_dropper, 0, 1, s2, Vec3{0, 0, 0});
  rx::u64 live_pid = live_world.Get<WorldItem>(live_item)->persistent_id;
  WorldItemStore empty_store;
  std::vector<rx::u8> live_blob = SaveWorldItems(live_world, empty_store);

  ecs::World reload;
  WorldItemStore reload_store;
  CHECK(LoadWorldItems(reload, physics, catalog, reload_store, live_blob) == true);
  CHECK(reload_store.size() == 0);
  int reloaded = 0;
  reload.Each<WorldItem>([&](ecs::Entity, WorldItem& wi) {
    ++reloaded;
    CHECK(wi.persistent_id == live_pid);
    CHECK(wi.body != 0);  // live items get a fresh body on load
  });
  CHECK(reloaded == 1);

  CHECK(LoadWorldItems(reload, physics, catalog, reload_store, {9, 9, 9}) == false);
}

// Regression: a blob that is valid up to a point and then truncated must be
// rejected with NO partial state applied -- pre-fix the loader committed each
// record as it parsed, so the first live item's entity + physics body survived
// a later failure, and an out-of-range serialized count reserved a huge buffer
// before failing.
void TestCorruptWorldItemRejection() {
  physics::PhysicsWorld physics;
  CHECK(physics.Initialize());
  physics.AddStaticBox(Vec3{0, 0, 0}, Vec3{10, 0.5f, 10});
  ItemCatalog catalog;
  ItemDefId rock = catalog.Register(3, MakeBoxDef(1.0f, 10, 1.0f));

  // Build a valid two-live-item blob.
  ecs::World src;
  ecs::Entity dropper = src.Create();
  Inventory inv;
  src.Add(dropper, inv);
  AddItem(*src.Get<Inventory>(dropper), catalog, rock, 2);
  scene::Transform s;
  s.position[1] = 2.0f;
  DropItem(src, physics, catalog, dropper, 0, 1, s, Vec3{0, 0, 0});
  DropItem(src, physics, catalog, dropper, 0, 1, s, Vec3{0, 0, 0});
  WorldItemStore empty;
  std::vector<u8> blob = SaveWorldItems(src, empty);
  CHECK(blob.size() > 16);

  const u32 bodies_before = physics.dynamic_body_count();
  // Truncate at every offset past the magic+version header (8 bytes) -- this
  // spans a chopped record count (which must fail the reserve bound instead of
  // allocating for the declared-but-absent records) and a chopped record body
  // (which must fail after full parse, before any commit). Every case must be
  // rejected with no entity and no store record created.
  for (size_t cut = 8; cut < blob.size(); ++cut) {
    std::vector<u8> truncated(blob.begin(), blob.begin() + cut);
    ecs::World dst;
    WorldItemStore store;
    CHECK(LoadWorldItems(dst, physics, catalog, store, truncated) == false);
    int live = 0;
    dst.Each<WorldItem>([&](ecs::Entity, WorldItem&) { ++live; });
    CHECK(live == 0);
    CHECK(store.size() == 0);
  }
  // No dynamic body leaked from any rejected load.
  CHECK(physics.dynamic_body_count() == bodies_before);
}

}  // namespace

int main() {
  TestStackingAndCapacity();
  TestRemoveAndTransfer();
  TestEquip();
  TestInventorySaveLoad();
  TestWorldItemLifecycle();
  TestHibernateWakeAndWorldSave();
  TestCorruptWorldItemRejection();

  if (g_failures == 0) {
    std::printf("inventory_test: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "inventory_test: %d checks failed\n", g_failures);
  return 1;
}
