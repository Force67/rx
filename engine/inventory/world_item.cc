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

// The physics layer exposes no body sleep-state or velocity query, so rest is
// inferred from how little the body moves between syncs. A settled/sleeping
// Jolt body reports an essentially unchanged transform, so the thresholds are
// kept tight (near float noise) rather than at a coarse per-call delta: at
// 60 Hz a 1 cm/call linear gate would latch anything moving below 0.6 m/s, and
// pure spin (unchanged position) would latch regardless. We gate on BOTH a
// small translation AND a small rotation change, sustained for kRestFrames.
constexpr f32 kRestLinearEpsilonSq = 1e-6f;  // ~1 mm of translation per call
constexpr f32 kRestAngularEpsilon = 1e-5f;   // 1 - |dot(q0, q1)|; ~0.5 deg/call
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

// Transactionally materialises a live world-item entity backed by a confirmed
// physics body. Creates NOTHING and returns kInvalidEntity when the def is
// unknown or the body could not be created (stub/uninitialised physics), so
// callers can roll back (leave the inventory or store untouched) instead of
// leaving a bodyless entity that never settles or hibernates.
ecs::Entity MaterializeWorldItem(ecs::World& world, physics::PhysicsWorld& physics,
                                 const ItemDef* def, const scene::Transform& t, ItemDefId item,
                                 u32 count, u64 payload, u64 persistent_id) {
  if (!def) return ecs::kInvalidEntity;
  physics::BodyId body = SpawnBody(physics, *def, t);
  if (body == 0) return ecs::kInvalidEntity;

  ecs::Entity e = world.Create();
  world.Add(e, t);
  if (def->world_mesh) world.Add(e, scene::Renderable{def->world_mesh});
  WorldItem wi;
  wi.item = item;
  wi.count = count;
  wi.payload = payload;
  wi.body = body;
  wi.persistent_id = persistent_id;
  SeedLastPos(wi, t);
  world.Add(e, wi);
  return e;
}

// "RXWI" v1.
constexpr u8 kMagic0 = 'R', kMagic1 = 'X', kMagic2 = 'W', kMagic3 = 'I';
constexpr u32 kVersion = 1;

// On-wire size of a serialized transform, and the minimum size of each record
// kind. Used to reject an implausible count before it is reserved so a corrupt
// blob fails cleanly instead of triggering an enormous allocation.
constexpr u32 kTransformBytes = 12 + 16 + 4;                     // pos, rot, scale
constexpr u32 kLiveRecordBytes = 8 + 4 + 4 + 8 + kTransformBytes + 1;  // + at_rest flag
constexpr u32 kDormantRecordBytes = 8 + 4 + 4 + 8 + kTransformBytes;

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

  // Materialise (and confirm) the world body BEFORE debiting the inventory: if
  // the body can't be created the drop fails with the inventory untouched,
  // rather than losing the item into a bodyless entity.
  ecs::Entity e =
      MaterializeWorldItem(world, physics, def, spawn, item, n, payload, NextWorldItemId());
  if (e == ecs::kInvalidEntity) return ecs::kInvalidEntity;

  entry.count -= n;
  if (entry.count == 0) {
    entry.item = kInvalidItemDef;
    entry.payload = 0;
  }
  ++inv->revision;

  if (impulse.x != 0 || impulse.y != 0 || impulse.z != 0)
    physics.ApplyImpulse(world.Get<WorldItem>(e)->body, impulse);
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

    // Angular change since the previous sync: t.rotation still holds last call's
    // orientation until we overwrite it below. 1 - |dot| grows with the rotation
    // between the two unit quaternions (|dot| handles the double-cover sign).
    const f32 rdot = t.rotation[0] * rot[0] + t.rotation[1] * rot[1] + t.rotation[2] * rot[2] +
                     t.rotation[3] * rot[3];
    const f32 ang_delta = 1.0f - std::fabs(rdot);

    t.position[0] = pos.x;
    t.position[1] = pos.y;
    t.position[2] = pos.z;
    for (int i = 0; i < 4; ++i) t.rotation[i] = rot[i];

    const f32 dx = pos.x - wi.last_pos[0];
    const f32 dy = pos.y - wi.last_pos[1];
    const f32 dz = pos.z - wi.last_pos[2];
    const bool still =
        dx * dx + dy * dy + dz * dz < kRestLinearEpsilonSq && ang_delta < kRestAngularEpsilon;
    if (still) {
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
    ecs::Entity e = MaterializeWorldItem(world, physics, def, rec.transform, rec.item, rec.count,
                                         rec.payload, rec.persistent_id);
    // Couldn't re-materialise a functioning body: keep the item dormant rather
    // than dropping the record on the floor, so it can be retried on a later
    // wake (and is never lost).
    if (e == ecs::kInvalidEntity) store.Insert(rec);
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

  // Parse+validate the WHOLE blob into bounded temporaries first, and only
  // commit (mint ids, create entities/bodies, insert store records) once it is
  // known good. A valid record followed by truncation or trailing garbage must
  // leave the world and store untouched, not half-loaded.
  u32 live = r.U32();
  if (!r.ok || live > r.Remaining() / kLiveRecordBytes) return false;
  std::vector<WorldItemRecord> live_recs;
  live_recs.reserve(live);
  for (u32 i = 0; i < live && r.ok; ++i) {
    WorldItemRecord rec;
    rec.persistent_id = r.U64();
    rec.item = r.U32();
    rec.count = r.U32();
    rec.payload = r.U64();
    rec.transform = ReadTransform(r);
    r.U8();  // at_rest flag: woken items re-settle, so it is not restored live
    if (!r.ok) break;
    live_recs.push_back(rec);
  }

  u32 dormant = r.U32();
  if (!r.ok || dormant > r.Remaining() / kDormantRecordBytes) return false;
  std::vector<WorldItemRecord> dorm_recs;
  dorm_recs.reserve(dormant);
  for (u32 i = 0; i < dormant && r.ok; ++i) {
    WorldItemRecord rec;
    rec.persistent_id = r.U64();
    rec.item = r.U32();
    rec.count = r.U32();
    rec.payload = r.U64();
    rec.transform = ReadTransform(r);
    if (!r.ok) break;
    dorm_recs.push_back(rec);
  }
  if (!r.ok || r.Remaining() != 0) return false;

  for (const auto& rec : live_recs) {
    ReserveWorldItemId(rec.persistent_id);
    const ItemDef* def = catalog.Find(rec.item);
    ecs::Entity e = MaterializeWorldItem(world, physics, def, rec.transform, rec.item, rec.count,
                                         rec.payload, rec.persistent_id);
    // No functioning body (stub physics / unknown def): keep the item as a
    // dormant record instead of losing it or leaving a bodyless entity.
    if (e == ecs::kInvalidEntity) store.Insert(rec);
  }
  for (const auto& rec : dorm_recs) {
    ReserveWorldItemId(rec.persistent_id);
    store.Insert(rec);
  }
  return true;
}

}  // namespace rx::inventory
