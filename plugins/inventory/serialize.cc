#include "plugins/inventory/serialize.h"

#include "base/containers/unordered_map.h"
#include "base/containers/vector.h"
#include "base/memory/move.h"
#include "plugins/inventory/byte_io.h"
#include "plugins/inventory/components.h"
#include "rxe/ecs/world.h"
#include "rxe/scene/components.h"

namespace rx::inventory {
namespace {

using namespace internal;

// "RXIN" v1.
constexpr u8 kMagic0 = 'R', kMagic1 = 'X', kMagic2 = 'I', kMagic3 = 'N';
constexpr u32 kVersion = 1;

// Smallest possible on-wire size of a serialized record, used to reject an
// implausible count before it is reserved (so a corrupt UINT32_MAX count fails
// cleanly instead of triggering an enormous allocation).
constexpr u32 kInventoryEntryBytes = 4 + 4 + 8;       // item, count, payload
constexpr u32 kEquipmentSlotBytes = 4 + 4 + 8 + 1;    // tag, item, payload, occupied
constexpr u32 kInventoryRecordMinBytes = 8 + 16 + 1;  // guid, inventory header, has_eq

void WriteInventory(base::Vector<u8>& b, const Inventory& inv) {
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
  if (!r.ok || n > r.Remaining() / kInventoryEntryBytes) {
    r.ok = false;
    return inv;
  }
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

void WriteEquipment(base::Vector<u8>& b, const Equipment& eq) {
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
  if (!r.ok || n > r.Remaining() / kEquipmentSlotBytes) {
    r.ok = false;
    return eq;
  }
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

base::Vector<u8> SaveInventories(ecs::World& world) {
  struct Record {
    u64 guid;
    Inventory inv;
    bool has_eq;
    Equipment eq;
  };
  base::Vector<Record> records;
  world.Each<scene::Guid, Inventory>([&](ecs::Entity e, scene::Guid& guid, Inventory& inv) {
    Record rec;
    rec.guid = guid.value;
    rec.inv = inv;
    Equipment* eq = world.Get<Equipment>(e);
    rec.has_eq = eq != nullptr;
    if (eq) rec.eq = *eq;
    records.push_back(base::move(rec));
  });

  base::Vector<u8> b;
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

bool LoadInventories(ecs::World& world, const base::Vector<u8>& blob) {
  Reader r(blob);
  if (r.U8() != kMagic0 || r.U8() != kMagic1 || r.U8() != kMagic2 || r.U8() != kMagic3) return false;
  if (r.U32() != kVersion) return false;

  u32 count = r.U32();
  if (!r.ok || count > r.Remaining() / kInventoryRecordMinBytes) return false;

  // Parse+validate fully into temporaries first; only commit to the world once
  // the whole blob is known good (no truncation, no trailing garbage). This
  // keeps a corrupt blob from leaving half the records applied.
  struct Parsed {
    u64 guid;
    Inventory inv;
    bool has_eq;
    Equipment eq;
  };
  base::Vector<Parsed> parsed;
  parsed.reserve(count);
  for (u32 i = 0; i < count && r.ok; ++i) {
    Parsed p;
    p.guid = r.U64();
    p.inv = ReadInventory(r);
    p.has_eq = r.U8() != 0;
    if (p.has_eq) p.eq = ReadEquipment(r);
    if (!r.ok) break;
    parsed.push_back(base::move(p));
  }
  if (!r.ok || r.Remaining() != 0) return false;

  base::UnorderedMap<u64, ecs::Entity> by_guid;
  world.Each<scene::Guid>([&](ecs::Entity e, scene::Guid& g) { by_guid[g.value] = e; });

  for (auto& rec : parsed) {
    ecs::Entity e;
    if (const ecs::Entity* found = by_guid.find(rec.guid)) {
      e = *found;
    } else {
      e = world.Create();
      world.Add(e, scene::Guid{rec.guid});
      by_guid[rec.guid] = e;
    }
    if (world.Has<Inventory>(e))
      *world.Get<Inventory>(e) = base::move(rec.inv);
    else
      world.Add(e, base::move(rec.inv));
    if (rec.has_eq) {
      if (world.Has<Equipment>(e))
        *world.Get<Equipment>(e) = base::move(rec.eq);
      else
        world.Add(e, base::move(rec.eq));
    }
  }
  return true;
}

}  // namespace rx::inventory
