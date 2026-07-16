#include "inventory/world_item.h"

#include <atomic>
#include <cmath>

#include "ecs/world.h"
#include "inventory/byte_io.h"
#include "inventory/inventory.h"
#include "physics/physics_world.h"

namespace rx::inventory {
namespace {

using namespace detail;

// The one piece of process-stateful module state: monotonic persistent-id
// minting. Kept off the functional path (systems still take no globals) and
// reconciled against loaded ids via ReserveWorldItemId.
std::atomic<u64> g_next_world_item_id{1};

// ~1 cm of movement between syncs counts as "still". A sleeping Jolt body
// reports an unchanged transform, so this reliably latches once it settles.
constexpr f32 kRestEpsilonSq = 1e-4f;
constexpr u16 kRestFrames = 6;

physics::BodyId SpawnBody(physics::PhysicsWorld& physics, const ItemDef& def,
                          const scene::Transform& t) {
  const Vec3 pos{t.position[0], t.position[1], t.position[2]};
  return physics.AddDynamicShape(def.shape, pos, t.rotation, def.scale, def.mass, def.friction,
                                 def.restitution);
}

void SeedLastPos(WorldItem& wi, const scene::Transform& t) {
  wi.last_pos[0] = t.position[0];
  wi.last_pos[1] = t.position[1];
  wi.last_pos[2] = t.position[2];
}

// "RXWI" v1.
constexpr u8 kMagic0 = 'R', kMagic1 = 'X', kMagic2 = 'W', kMagic3 = 'I';
constexpr u32 kVersion = 1;

void WriteTransform(std::vector<u8>& b, const scene::Transform& t) {
  for (int i = 0; i < 3; ++i) PutF32(b, t.position[i]);
  for (int i = 0; i < 4; ++i) PutF32(b, t.rotation[i]);
  PutF32(b, t.scale);
}

scene::Transform ReadTransform(Reader& r) {
  scene::Transform t;
  for (int i = 0; i < 3; ++i) t.position[i] = r.F32();
  for (int i = 0; i < 4; ++i) t.rotation[i] = r.F32();
  t.scale = r.F32();
  return t;
}

}  // namespace

u64 NextWorldItemId() { return g_next_world_item_id.fetch_add(1); }

void ReserveWorldItemId(u64 seen_id) {
  u64 cur = g_next_world_item_id.load();
  while (seen_id >= cur && !g_next_world_item_id.compare_exchange_weak(cur, seen_id + 1)) {
    // cur reloaded by compare_exchange_weak on failure.
  }
}

// --- WorldItemStore ---

size_t WorldItemStore::CellKeyHash::operator()(const CellKey& key) const {
  u64 h = 1469598103934665603ull;  // FNV-1a 64 offset basis
  auto mix = [&](i32 v) {
    h ^= u32(v);
    h *= 1099511628211ull;
  };
  mix(key.x);
  mix(key.y);
  mix(key.z);
  return size_t(h);
}

WorldItemStore::CellKey WorldItemStore::CellOf(const Vec3& p) const {
  return CellKey{i32(std::floor(p.x / cell_size_)), i32(std::floor(p.y / cell_size_)),
                 i32(std::floor(p.z / cell_size_))};
}

void WorldItemStore::Insert(const WorldItemRecord& record) {
  const Vec3 p{record.transform.position[0], record.transform.position[1],
               record.transform.position[2]};
  cells_[CellOf(p)].push_back(record);
  ++count_;
}

std::vector<WorldItemRecord> WorldItemStore::TakeNear(const Vec3& center, f32 radius) {
  std::vector<WorldItemRecord> out;
  const i32 span = i32(std::ceil(radius / cell_size_));
  const CellKey c = CellOf(center);
  for (i32 z = c.z - span; z <= c.z + span; ++z) {
    for (i32 y = c.y - span; y <= c.y + span; ++y) {
      for (i32 x = c.x - span; x <= c.x + span; ++x) {
        auto it = cells_.find(CellKey{x, y, z});
        if (it == cells_.end()) continue;
        for (const auto& rec : it->second) out.push_back(rec);
        count_ -= it->second.size();
        cells_.erase(it);
      }
    }
  }
  return out;
}

void WorldItemStore::ForEach(const std::function<void(const WorldItemRecord&)>& fn) const {
  for (const auto& [key, bucket] : cells_)
    for (const auto& rec : bucket) fn(rec);
}

void WorldItemStore::Clear() {
  cells_.clear();
  count_ = 0;
}

// --- drop / pick up ---

ecs::Entity DropItem(ecs::World& world, physics::PhysicsWorld& physics, const ItemCatalog& catalog,
                     ecs::Entity source, u32 entry_index, u32 count, const scene::Transform& spawn,
                     const Vec3& impulse) {
  Inventory* inv = world.Get<Inventory>(source);
  if (!inv || entry_index >= inv->entries.size()) return ecs::kInvalidEntity;
  InventoryEntry& entry = inv->entries[entry_index];
  if (entry.count == 0 || entry.item == kInvalidItemDef) return ecs::kInvalidEntity;
  const ItemDef* def = catalog.Find(entry.item);
  if (!def) return ecs::kInvalidEntity;

  u32 n = count < entry.count ? count : entry.count;
  if (n == 0) return ecs::kInvalidEntity;

  const ItemDefId item = entry.item;
  const u64 payload = entry.payload;
  entry.count -= n;
  if (entry.count == 0) {
    entry.item = kInvalidItemDef;
    entry.payload = 0;
  }
  ++inv->revision;

  ecs::Entity e = world.Create();
  world.Add(e, spawn);
  if (def->world_mesh) world.Add(e, scene::Renderable{def->world_mesh});

  physics::BodyId body = SpawnBody(physics, *def, spawn);
  if (body != 0 && (impulse.x != 0 || impulse.y != 0 || impulse.z != 0))
    physics.ApplyImpulse(body, impulse);

  WorldItem wi;
  wi.item = item;
  wi.count = n;
  wi.payload = payload;
  wi.body = body;
  wi.persistent_id = NextWorldItemId();
  SeedLastPos(wi, spawn);
  world.Add(e, wi);
  return e;
}

bool PickUpItem(ecs::World& world, physics::PhysicsWorld& physics, const ItemCatalog& catalog,
                ecs::Entity item_entity, ecs::Entity receiver) {
  WorldItem* wi = world.Get<WorldItem>(item_entity);
  if (!wi) return false;
  Inventory* inv = world.Get<Inventory>(receiver);
  if (!inv) return false;

  u32 added = AddItem(*inv, catalog, wi->item, wi->count, wi->payload);
  if (added == 0) return false;
  if (added < wi->count) {
    wi->count -= added;  // partial pickup: the rest stays in the world
    return false;
  }
  if (wi->body != 0) physics.RemoveBody(wi->body);
  world.Destroy(item_entity);
  return true;
}

// --- per-tick maintenance ---

void SyncWorldItems(ecs::World& world, physics::PhysicsWorld& physics) {
  world.Each<scene::Transform, WorldItem>([&](ecs::Entity, scene::Transform& t, WorldItem& wi) {
    if (wi.at_rest || wi.body == 0) return;
    Vec3 pos;
    f32 rot[4];
    if (!physics.GetBodyTransform(wi.body, &pos, rot)) return;
    t.position[0] = pos.x;
    t.position[1] = pos.y;
    t.position[2] = pos.z;
    for (int i = 0; i < 4; ++i) t.rotation[i] = rot[i];

    const f32 dx = pos.x - wi.last_pos[0];
    const f32 dy = pos.y - wi.last_pos[1];
    const f32 dz = pos.z - wi.last_pos[2];
    if (dx * dx + dy * dy + dz * dz < kRestEpsilonSq) {
      if (wi.still_frames < 0xffff) ++wi.still_frames;
      if (wi.still_frames >= kRestFrames) wi.at_rest = true;
    } else {
      wi.still_frames = 0;
    }
    wi.last_pos[0] = pos.x;
    wi.last_pos[1] = pos.y;
    wi.last_pos[2] = pos.z;
  });
}

void HibernateDistantWorldItems(ecs::World& world, physics::PhysicsWorld& physics,
                                WorldItemStore& store, const Vec3& center, f32 radius) {
  const f32 r2 = radius * radius;
  // Collect first: mutating the ECS (Destroy) mid-Each can skip/revisit rows.
  std::vector<ecs::Entity> victims;
  std::vector<WorldItemRecord> records;
  world.Each<scene::Transform, WorldItem>([&](ecs::Entity e, scene::Transform& t, WorldItem& wi) {
    if (!wi.at_rest) return;
    const f32 dx = t.position[0] - center.x;
    const f32 dy = t.position[1] - center.y;
    const f32 dz = t.position[2] - center.z;
    if (dx * dx + dy * dy + dz * dz <= r2) return;
    WorldItemRecord rec;
    rec.persistent_id = wi.persistent_id;
    rec.item = wi.item;
    rec.count = wi.count;
    rec.payload = wi.payload;
    rec.transform = t;
    records.push_back(rec);
    victims.push_back(e);
    if (wi.body != 0) physics.RemoveBody(wi.body);
  });
  for (const auto& rec : records) store.Insert(rec);
  for (ecs::Entity e : victims) world.Destroy(e);
}

void WakeWorldItemsNear(ecs::World& world, physics::PhysicsWorld& physics,
                        const ItemCatalog& catalog, WorldItemStore& store, const Vec3& center,
                        f32 radius) {
  std::vector<WorldItemRecord> woken = store.TakeNear(center, radius);
  for (const auto& rec : woken) {
    const ItemDef* def = catalog.Find(rec.item);
    ecs::Entity e = world.Create();
    world.Add(e, rec.transform);
    if (def && def->world_mesh) world.Add(e, scene::Renderable{def->world_mesh});
    WorldItem wi;
    wi.item = rec.item;
    wi.count = rec.count;
    wi.payload = rec.payload;
    wi.persistent_id = rec.persistent_id;
    SeedLastPos(wi, rec.transform);
    if (def) wi.body = SpawnBody(physics, *def, rec.transform);
    world.Add(e, wi);
  }
}

// --- persistence (world items) ---

std::vector<u8> SaveWorldItems(ecs::World& world, const WorldItemStore& store) {
  struct Live {
    scene::Transform transform;
    ItemDefId item;
    u32 count;
    u64 payload;
    u64 persistent_id;
    u8 at_rest;
  };
  std::vector<Live> live;
  world.Each<scene::Transform, WorldItem>([&](ecs::Entity, scene::Transform& t, WorldItem& wi) {
    live.push_back(Live{t, wi.item, wi.count, wi.payload, wi.persistent_id, u8(wi.at_rest ? 1 : 0)});
  });
  std::vector<WorldItemRecord> dormant;
  store.ForEach([&](const WorldItemRecord& rec) { dormant.push_back(rec); });

  std::vector<u8> b;
  b.push_back(kMagic0);
  b.push_back(kMagic1);
  b.push_back(kMagic2);
  b.push_back(kMagic3);
  PutU32(b, kVersion);

  PutU32(b, u32(live.size()));
  for (const auto& l : live) {
    PutU64(b, l.persistent_id);
    PutU32(b, l.item);
    PutU32(b, l.count);
    PutU64(b, l.payload);
    WriteTransform(b, l.transform);
    PutU8(b, l.at_rest);
  }
  PutU32(b, u32(dormant.size()));
  for (const auto& rec : dormant) {
    PutU64(b, rec.persistent_id);
    PutU32(b, rec.item);
    PutU32(b, rec.count);
    PutU64(b, rec.payload);
    WriteTransform(b, rec.transform);
  }
  return b;
}

bool LoadWorldItems(ecs::World& world, physics::PhysicsWorld& physics, const ItemCatalog& catalog,
                    WorldItemStore& store, const std::vector<u8>& blob) {
  Reader r(blob);
  if (r.U8() != kMagic0 || r.U8() != kMagic1 || r.U8() != kMagic2 || r.U8() != kMagic3) return false;
  if (r.U32() != kVersion) return false;

  u32 live = r.U32();
  for (u32 i = 0; i < live && r.ok; ++i) {
    u64 pid = r.U64();
    ItemDefId item = r.U32();
    u32 count = r.U32();
    u64 payload = r.U64();
    scene::Transform t = ReadTransform(r);
    r.U8();  // at_rest flag: woken items re-settle, so it is not restored live
    if (!r.ok) break;
    ReserveWorldItemId(pid);

    const ItemDef* def = catalog.Find(item);
    ecs::Entity e = world.Create();
    world.Add(e, t);
    if (def && def->world_mesh) world.Add(e, scene::Renderable{def->world_mesh});
    WorldItem wi;
    wi.item = item;
    wi.count = count;
    wi.payload = payload;
    wi.persistent_id = pid;
    SeedLastPos(wi, t);
    if (def) wi.body = SpawnBody(physics, *def, t);
    world.Add(e, wi);
  }

  u32 dormant = r.U32();
  for (u32 i = 0; i < dormant && r.ok; ++i) {
    WorldItemRecord rec;
    rec.persistent_id = r.U64();
    rec.item = r.U32();
    rec.count = r.U32();
    rec.payload = r.U64();
    rec.transform = ReadTransform(r);
    if (!r.ok) break;
    ReserveWorldItemId(rec.persistent_id);
    store.Insert(rec);
  }
  return r.ok;
}

}  // namespace rx::inventory
