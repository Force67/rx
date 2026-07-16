#include "inventory/serialize.h"

#include <unordered_map>

#include "ecs/world.h"
#include "inventory/byte_io.h"
#include "inventory/components.h"
#include "scene/components.h"

namespace rx::inventory {
namespace {

using namespace detail;

// "RXIN" v1.
constexpr u8 kMagic0 = 'R', kMagic1 = 'X', kMagic2 = 'I', kMagic3 = 'N';
constexpr u32 kVersion = 1;

void WriteInventory(std::vector<u8>& b, const Inventory& inv) {
  PutF32(b, inv.max_weight);
  PutU32(b, inv.max_entries);
  PutU32(b, inv.revision);
  // Only non-empty entries are persisted; empty reuse slots are transient.
  u32 n = 0;
  for (const auto& e : inv.entries)
    if (e.count > 0) ++n;
  PutU32(b, n);
  for (const auto& e : inv.entries) {
    if (e.count == 0) continue;
    PutU32(b, e.item);
    PutU32(b, e.count);
    PutU64(b, e.payload);
  }
}

Inventory ReadInventory(Reader& r) {
  Inventory inv;
  inv.max_weight = r.F32();
  inv.max_entries = r.U32();
  inv.revision = r.U32();
  u32 n = r.U32();
  inv.entries.reserve(n);
  for (u32 i = 0; i < n && r.ok; ++i) {
    InventoryEntry e;
    e.item = r.U32();
    e.count = r.U32();
    e.payload = r.U64();
    inv.entries.push_back(e);
  }
  return inv;
}

void WriteEquipment(std::vector<u8>& b, const Equipment& eq) {
  PutU32(b, eq.revision);
  PutU32(b, u32(eq.slots.size()));
  for (const auto& s : eq.slots) {
    PutU32(b, s.tag);
    PutU32(b, s.item);
    PutU64(b, s.payload);
    PutU8(b, s.occupied ? 1 : 0);
  }
}

Equipment ReadEquipment(Reader& r) {
  Equipment eq;
  eq.revision = r.U32();
  u32 n = r.U32();
  eq.slots.reserve(n);
  for (u32 i = 0; i < n && r.ok; ++i) {
    EquipmentSlot s;
    s.tag = r.U32();
    s.item = r.U32();
    s.payload = r.U64();
    s.occupied = r.U8() != 0;
    eq.slots.push_back(s);
  }
  return eq;
}

}  // namespace

std::vector<u8> SaveInventories(ecs::World& world) {
  struct Record {
    u64 guid;
    Inventory inv;
    bool has_eq;
    Equipment eq;
  };
  std::vector<Record> records;
  world.Each<scene::Guid, Inventory>([&](ecs::Entity e, scene::Guid& guid, Inventory& inv) {
    Record rec;
    rec.guid = guid.value;
    rec.inv = inv;
    Equipment* eq = world.Get<Equipment>(e);
    rec.has_eq = eq != nullptr;
    if (eq) rec.eq = *eq;
    records.push_back(std::move(rec));
  });

  std::vector<u8> b;
  b.push_back(kMagic0);
  b.push_back(kMagic1);
  b.push_back(kMagic2);
  b.push_back(kMagic3);
  PutU32(b, kVersion);
  PutU32(b, u32(records.size()));
  for (const auto& rec : records) {
    PutU64(b, rec.guid);
    WriteInventory(b, rec.inv);
    PutU8(b, rec.has_eq ? 1 : 0);
    if (rec.has_eq) WriteEquipment(b, rec.eq);
  }
  return b;
}

bool LoadInventories(ecs::World& world, const std::vector<u8>& blob) {
  Reader r(blob);
  if (r.U8() != kMagic0 || r.U8() != kMagic1 || r.U8() != kMagic2 || r.U8() != kMagic3) return false;
  if (r.U32() != kVersion) return false;

  std::unordered_map<u64, ecs::Entity> by_guid;
  world.Each<scene::Guid>([&](ecs::Entity e, scene::Guid& g) { by_guid[g.value] = e; });

  u32 count = r.U32();
  for (u32 i = 0; i < count && r.ok; ++i) {
    u64 guid = r.U64();
    Inventory inv = ReadInventory(r);
    bool has_eq = r.U8() != 0;
    Equipment eq;
    if (has_eq) eq = ReadEquipment(r);
    if (!r.ok) break;

    ecs::Entity e;
    auto it = by_guid.find(guid);
    if (it != by_guid.end()) {
      e = it->second;
    } else {
      e = world.Create();
      world.Add(e, scene::Guid{guid});
      by_guid[guid] = e;
    }
    if (world.Has<Inventory>(e))
      *world.Get<Inventory>(e) = std::move(inv);
    else
      world.Add(e, std::move(inv));
    if (has_eq) {
      if (world.Has<Equipment>(e))
        *world.Get<Equipment>(e) = std::move(eq);
      else
        world.Add(e, std::move(eq));
    }
  }
  return r.ok;
}

}  // namespace rx::inventory
