// The baked map formats: round trip, the cook-time consistency checks that keep
// a broken world from ever reaching an archive, and the load-time checks that
// make a corrupted or stale one fail loudly instead of materializing garbage.
#include "world/world_format.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

using namespace rx::world;
using rx::f32;
using rx::u32;
using rx::u64;
using rx::u8;
using rx::Quat;
using rx::Vec3;

int g_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

void CheckRejected(bool decoded, const std::string& error, const char* what) {
  if (decoded) {
    std::fprintf(stderr, "FAIL: %s was accepted\n", what);
    ++g_failures;
    return;
  }
  if (error.empty()) {
    std::fprintf(stderr, "FAIL: %s was rejected without a message\n", what);
    ++g_failures;
  }
}

struct Position {
  f32 x = 0, y = 0, z = 0;
};

constexpr u64 kBakeId = 0x0123456789abcdefull;

base::Vector<u8> BytesOf(const void* data, size_t size) {
  base::Vector<u8> bytes;
  const u8* source = static_cast<const u8*>(data);
  bytes.insert(bytes.end(), source, source + size);
  return bytes;
}

void TestPayloadPathIsDerived() {
  CHECK(CellPayloadPath("world/city", 0x2a, Domain::kGameplay, Tier::kStandard) ==
        "world/city/000000000000002a.gameplay.standard.rxcell");
  // A prefix that already ends in a separator must not grow a second one.
  CHECK(CellPayloadPath("world/city/", 1, Domain::kCollision, Tier::kProxy) ==
        "world/city/0000000000000001.collision.proxy.rxcell");
  CHECK(CellPayloadPath("", 0, Domain::kAudio, Tier::kFull) ==
        "0000000000000000.audio.full.rxcell");
}

void TestIndexRoundTrip() {
  WorldIndexWriter writer;
  writer.set_world_id(0xfeedu);
  writer.set_bake_id(kBakeId);
  writer.set_grid(64.0f, {-512, 0, -512});
  // Deliberately out of id order, and with inverted bounds on one cell.
  writer.AddCell(7, {64, 0, 0}, {128, 32, 64}, 1, 700, 10);
  writer.AddCell(3, {64, 32, 64}, {0, 0, 0}, 0, 300, 10);
  writer.AddPayload(3, Domain::kGameplay, Tier::kStandard, 4096, 12);
  writer.AddPayload(3, Domain::kRepresentation, Tier::kFull, 8192, 900);
  writer.AddPayload(3, Domain::kGameplay, Tier::kProxy, 512, 2);
  writer.AddPayload(7, Domain::kCollision, Tier::kStandard, 2048, 1);
  // Re-adding one replaces it rather than duplicating it.
  writer.AddPayload(3, Domain::kGameplay, Tier::kStandard, 5000, 13);

  base::Vector<u8> bytes;
  std::string error;
  CHECK(writer.Encode(&bytes, &error));
  CHECK(error.empty());

  WorldIndexData index;
  CHECK(DecodeWorldIndex(std::span<const u8>(bytes.data(), bytes.size()), &index, &error));
  CHECK(error.empty());
  CHECK(index.world_id == 0xfeedu);
  CHECK(index.bake_id == kBakeId);
  CHECK(index.cell_size == 64.0f);
  CHECK(index.cells.size() == 2);
  CHECK(index.payloads.size() == 4);

  // Sorted by id regardless of the order they were added in.
  CHECK(index.cells[0].id == 3);
  CHECK(index.cells[1].id == 7);

  const WorldCellRecord* cell = index.FindCell(3);
  CHECK(cell != nullptr);
  if (!cell) return;
  // The inverted bounds were canonicalized at cook time.
  CHECK(cell->minimum.x == 0 && cell->maximum.x == 64);
  CHECK(cell->minimum.y == 0 && cell->maximum.y == 32);
  CHECK(cell->payload_count == 3);

  const WorldPayloadRecord* gameplay = index.FindPayload(*cell, Domain::kGameplay, Tier::kStandard);
  CHECK(gameplay != nullptr);
  CHECK(gameplay && gameplay->resident_bytes == 5000);
  CHECK(gameplay && gameplay->row_count == 13);
  CHECK(index.FindPayload(*cell, Domain::kNavigation, Tier::kStandard) == nullptr);

  // BestTier picks the highest the cook produced at or below the ceiling.
  CHECK(index.BestTier(*cell, Domain::kGameplay, Tier::kFull) == Tier::kStandard);
  CHECK(index.BestTier(*cell, Domain::kGameplay, Tier::kProxy) == Tier::kProxy);
  CHECK(index.BestTier(*cell, Domain::kRepresentation, Tier::kStandard) == Tier::kAbsent);
  CHECK(index.BestTier(*cell, Domain::kAudio, Tier::kFull) == Tier::kAbsent);

  // A stable id resolves to its owning cell with nothing resident.
  CHECK(index.FindCellByStableId(305) == index.FindCell(3));
  CHECK(index.FindCellByStableId(705) == index.FindCell(7));
  CHECK(index.FindCellByStableId(310) == nullptr);  // past cell 3's range
  CHECK(index.FindCellByStableId(299) == nullptr);  // before it
  CHECK(index.FindCellByStableId(0) == nullptr);
}

// Cells are sorted by cell id; nothing makes the cook hand out stable-id ranges
// in that same order. Resolving an id must not assume the two agree, and cells
// that own no ids at all must not sit in the way of ones that do.
void TestStableIdLookupIgnoresCellOrder() {
  WorldIndexWriter writer;
  // Descending ranges against ascending ids, plus two cells owning nothing,
  // one of them with a stable_id_first inside another cell's range.
  writer.AddCell(1, {}, {1, 1, 1}, 0, 900, 100);
  writer.AddCell(2, {}, {1, 1, 1}, 0, 500, 100);
  writer.AddCell(3, {}, {1, 1, 1}, 0, 950, 0);  // owns nothing, sits inside cell 1's range
  writer.AddCell(4, {}, {1, 1, 1}, 0, 100, 100);
  writer.AddCell(5, {}, {1, 1, 1}, 0, 0, 0);  // owns nothing, at the bottom
  writer.AddPayload(1, Domain::kGameplay, Tier::kStandard, 16, 1);

  base::Vector<u8> bytes;
  std::string error;
  CHECK(writer.Encode(&bytes, &error));
  CHECK(error.empty());

  WorldIndexData index;
  CHECK(DecodeWorldIndex(std::span<const u8>(bytes.data(), bytes.size()), &index, &error));
  CHECK(index.stable_id_order.size() == 3);  // only the cells that own ids

  CHECK(index.FindCellByStableId(950) == index.FindCell(1));
  CHECK(index.FindCellByStableId(999) == index.FindCell(1));
  CHECK(index.FindCellByStableId(550) == index.FindCell(2));
  CHECK(index.FindCellByStableId(150) == index.FindCell(4));
  CHECK(index.FindCellByStableId(100) == index.FindCell(4));
  // Between and beyond the ranges.
  CHECK(index.FindCellByStableId(0) == nullptr);
  CHECK(index.FindCellByStableId(99) == nullptr);
  CHECK(index.FindCellByStableId(200) == nullptr);
  CHECK(index.FindCellByStableId(1000) == nullptr);
}

void TestIndexRefusesInconsistentWorlds() {
  std::string error;
  base::Vector<u8> bytes;
  {
    WorldIndexWriter writer;
    writer.AddCell(1, {}, {1, 1, 1}, 0, 0, 0);
    writer.AddPayload(2, Domain::kGameplay, Tier::kStandard, 16, 1);
    CheckRejected(writer.Encode(&bytes, &error), error, "a payload naming an unknown cell");
  }
  {
    WorldIndexWriter writer;
    writer.AddCell(1, {}, {1, 1, 1}, 0, 0, 0);
    writer.AddPayload(1, Domain::kGameplay, Tier::kAbsent, 16, 1);
    CheckRejected(writer.Encode(&bytes, &error), error, "a payload baked at tier absent");
  }
  {
    // The check that matters most: overlapping ranges would silently resolve a
    // stable id to the wrong cell forever.
    WorldIndexWriter writer;
    writer.AddCell(1, {}, {1, 1, 1}, 0, 100, 50);
    writer.AddCell(2, {}, {1, 1, 1}, 0, 120, 50);
    CheckRejected(writer.Encode(&bytes, &error), error, "overlapping stable-id ranges");
  }
  {
    WorldIndexWriter writer;
    writer.AddCell(1, {}, {1, 1, 1}, 0, ~u64{0} - 2, 10);
    CheckRejected(writer.Encode(&bytes, &error), error, "a stable-id range that wraps");
  }
  {
    // Abutting ranges are legal: [100,150) then [150,200).
    WorldIndexWriter writer;
    writer.AddCell(1, {}, {1, 1, 1}, 0, 100, 50);
    writer.AddCell(2, {}, {1, 1, 1}, 0, 150, 50);
    CHECK(writer.Encode(&bytes, &error));
  }
}

void TestIndexRefusesCorruptedBytes() {
  WorldIndexWriter writer;
  writer.set_bake_id(kBakeId);
  writer.AddCell(1, {}, {16, 16, 16}, 0, 0, 4);
  writer.AddCell(2, {16, 0, 0}, {32, 16, 16}, 0, 4, 4);
  writer.AddPayload(1, Domain::kGameplay, Tier::kStandard, 64, 4);
  base::Vector<u8> good;
  std::string error;
  CHECK(writer.Encode(&good, &error));

  WorldIndexData index;
  {
    base::Vector<u8> bad(good);
    bad[0] = 'X';
    CheckRejected(DecodeWorldIndex(std::span<const u8>(bad.data(), bad.size()), &index, &error),
                  error, "a bad magic");
  }
  {
    base::Vector<u8> bad(good);
    bad[8] = 9;  // version
    CheckRejected(DecodeWorldIndex(std::span<const u8>(bad.data(), bad.size()), &index, &error),
                  error, "a future version");
  }
  {
    // Flip a byte in the body: the checksum must catch it.
    base::Vector<u8> bad(good);
    bad[bad.size() - 1] ^= 0xff;
    CheckRejected(DecodeWorldIndex(std::span<const u8>(bad.data(), bad.size()), &index, &error),
                  error, "a flipped body byte");
  }
  {
    base::Vector<u8> bad(good);
    bad.erase(bad.end() - 1);
    CheckRejected(DecodeWorldIndex(std::span<const u8>(bad.data(), bad.size()), &index, &error),
                  error, "a truncated body");
  }
  {
    base::Vector<u8> bad;
    CheckRejected(DecodeWorldIndex(std::span<const u8>(bad.data(), bad.size()), &index, &error),
                  error, "an empty file");
  }
  CHECK(DecodeWorldIndex(std::span<const u8>(good.data(), good.size()), &index, &error));
}

void TestEntityPayloadRoundTrip() {
  const Position positions[3] = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}};
  const u64 ids[3] = {10, 11, 12};
  const u64 layout = HashComponentLayout("Position", sizeof(Position), {}, {}, {});

  CellPayloadWriter writer(42, Domain::kGameplay, Tier::kStandard);
  writer.set_bake_id(kBakeId);
  const u32 archetype = writer.BeginArchetype(3);
  const base::Vector<u8> column = BytesOf(positions, sizeof(positions));
  writer.AddColumn(archetype, "Position", sizeof(Position), layout,
                   std::span<const u8>(column.data(), column.size()));
  writer.SetStableIds(archetype, std::span<const u64>(ids, 3));

  base::Vector<u8> bytes;
  std::string error;
  CHECK(writer.Encode(&bytes, &error));
  CHECK(error.empty());

  WorldCellPayload payload;
  CHECK(DecodeCellPayload(std::span<const u8>(bytes.data(), bytes.size()), &payload, &error));
  CHECK(payload.kind == PayloadKind::kEntities);
  CHECK(payload.cell_id == 42);
  CHECK(payload.bake_id == kBakeId);
  CHECK(payload.domain == Domain::kGameplay);
  CHECK(payload.tier == Tier::kStandard);
  CHECK(payload.archetypes.size() == 1);
  CHECK(payload.columns.size() == 1);
  CHECK(payload.total_row_count() == 3);
  if (payload.archetypes.empty() || payload.columns.empty()) return;

  CHECK(payload.archetypes[0].row_count == 3);
  CHECK(payload.String(payload.columns[0].name) == "Position");
  CHECK(payload.columns[0].stride == sizeof(Position));
  CHECK(payload.columns[0].layout_hash == layout);

  const std::span<const u8> decoded = payload.ColumnBytes(payload.columns[0]);
  CHECK(decoded.size() == sizeof(positions));
  CHECK(std::memcmp(decoded.data(), positions, sizeof(positions)) == 0);

  const std::span<const u64> decoded_ids = payload.StableIds(payload.archetypes[0]);
  CHECK(decoded_ids.size() == 3);
  CHECK(decoded_ids.size() == 3 && decoded_ids[0] == 10 && decoded_ids[2] == 12);
}

void TestInstancePayloadRoundTrip() {
  CellPayloadWriter writer(9, Domain::kRepresentation, Tier::kFull);
  writer.set_bake_id(kBakeId);
  const u32 rock = writer.AddPrototype("prop/rock");
  const u32 tree = writer.AddPrototype("prop/tree");
  CHECK(writer.AddPrototype("prop/rock") == rock);  // interned, not duplicated
  writer.AddInstance(500, rock, {1, 2, 3}, {0, 0, 0, 1}, 1.5f);
  writer.AddInstance(501, tree, {4, 5, 6}, {0, 0.7071f, 0, 0.7071f}, 2.0f);

  base::Vector<u8> bytes;
  std::string error;
  CHECK(writer.Encode(&bytes, &error));

  WorldCellPayload payload;
  CHECK(DecodeCellPayload(std::span<const u8>(bytes.data(), bytes.size()), &payload, &error));
  CHECK(payload.kind == PayloadKind::kInstances);
  CHECK(payload.prototypes.size() == 2);
  CHECK(payload.instances.size() == 2);
  CHECK(payload.total_row_count() == 2);
  if (payload.instances.size() != 2 || payload.prototypes.size() != 2) return;
  CHECK(payload.String(payload.prototypes[0].name) == "prop/rock");
  CHECK(payload.instances[0].stable_id == 500);
  CHECK(payload.instances[1].prototype == tree);
  CHECK(payload.instances[1].scale == 2.0f);
  CHECK(payload.instances[0].position.y == 2);
}

void TestPayloadRefusesInconsistentContent() {
  base::Vector<u8> bytes;
  std::string error;
  const u64 layout = HashComponentLayout("Position", sizeof(Position), {}, {}, {});
  const Position positions[2] = {{}, {}};
  const base::Vector<u8> two_rows = BytesOf(positions, sizeof(positions));
  const u64 ids[2] = {1, 2};

  {
    CellPayloadWriter writer(1, Domain::kGameplay, Tier::kStandard);
    const u32 archetype = writer.BeginArchetype(3);  // three rows, two rows of bytes
    writer.AddColumn(archetype, "Position", sizeof(Position), layout,
                     std::span<const u8>(two_rows.data(), two_rows.size()));
    const u64 three[3] = {1, 2, 3};
    writer.SetStableIds(archetype, std::span<const u64>(three, 3));
    CheckRejected(writer.Encode(&bytes, &error), error, "a column short of its row count");
  }
  {
    CellPayloadWriter writer(1, Domain::kGameplay, Tier::kStandard);
    const u32 archetype = writer.BeginArchetype(2);
    writer.AddColumn(archetype, "Position", sizeof(Position), layout,
                     std::span<const u8>(two_rows.data(), two_rows.size()));
    CheckRejected(writer.Encode(&bytes, &error), error, "an archetype with no stable ids");
  }
  {
    CellPayloadWriter writer(1, Domain::kGameplay, Tier::kStandard);
    const u32 archetype = writer.BeginArchetype(2);
    writer.AddColumn(archetype, "Position", sizeof(Position), layout,
                     std::span<const u8>(two_rows.data(), two_rows.size()));
    writer.AddColumn(archetype, "Position", sizeof(Position), layout,
                     std::span<const u8>(two_rows.data(), two_rows.size()));
    writer.SetStableIds(archetype, std::span<const u64>(ids, 2));
    CheckRejected(writer.Encode(&bytes, &error), error, "one component listed twice");
  }
  {
    CellPayloadWriter writer(1, Domain::kGameplay, Tier::kStandard);
    const u32 archetype = writer.BeginArchetype(2);
    writer.AddColumn(archetype, "Position", sizeof(Position), layout,
                     std::span<const u8>(two_rows.data(), two_rows.size()));
    const u64 duplicated[2] = {5, 5};
    writer.SetStableIds(archetype, std::span<const u64>(duplicated, 2));
    CheckRejected(writer.Encode(&bytes, &error), error, "a duplicated stable id");
  }
  {
    CellPayloadWriter writer(1, Domain::kGameplay, Tier::kStandard);
    const u32 archetype = writer.BeginArchetype(2);
    writer.AddColumn(archetype, "Position", sizeof(Position), layout,
                     std::span<const u8>(two_rows.data(), two_rows.size()));
    writer.SetStableIds(archetype, std::span<const u64>(ids, 2));
    writer.AddInstance(1, writer.AddPrototype("prop/rock"), {}, {}, 1);
    CheckRejected(writer.Encode(&bytes, &error), error, "entity and instance content mixed");
  }
}

void TestPayloadRefusesCorruptedBytes() {
  const Position positions[2] = {{1, 2, 3}, {4, 5, 6}};
  const base::Vector<u8> column = BytesOf(positions, sizeof(positions));
  const u64 ids[2] = {1, 2};
  CellPayloadWriter writer(1, Domain::kGameplay, Tier::kStandard);
  writer.set_bake_id(kBakeId);
  const u32 archetype = writer.BeginArchetype(2);
  writer.AddColumn(archetype, "Position", sizeof(Position),
                   HashComponentLayout("Position", sizeof(Position), {}, {}, {}),
                   std::span<const u8>(column.data(), column.size()));
  writer.SetStableIds(archetype, std::span<const u64>(ids, 2));
  base::Vector<u8> good;
  std::string error;
  CHECK(writer.Encode(&good, &error));

  WorldCellPayload payload;
  {
    base::Vector<u8> bad(good);
    bad[3] = 'X';
    CheckRejected(DecodeCellPayload(std::span<const u8>(bad.data(), bad.size()), &payload, &error),
                  error, "a payload with a bad magic");
  }
  {
    // The checksum covers the data section, which is the half that would
    // otherwise be copied into an entity column verbatim.
    base::Vector<u8> bad(good);
    bad[bad.size() - 5] ^= 0x01;
    CheckRejected(DecodeCellPayload(std::span<const u8>(bad.data(), bad.size()), &payload, &error),
                  error, "a flipped byte in the data section");
  }
  {
    base::Vector<u8> bad(good);
    bad.erase(bad.end() - 4);
    CheckRejected(DecodeCellPayload(std::span<const u8>(bad.data(), bad.size()), &payload, &error),
                  error, "a truncated payload");
  }
  {
    base::Vector<u8> bad;
    bad.insert(bad.end(), good.begin(), good.begin() + 10);
    CheckRejected(DecodeCellPayload(std::span<const u8>(bad.data(), bad.size()), &payload, &error),
                  error, "a payload cut off inside its header");
  }
  CHECK(DecodeCellPayload(std::span<const u8>(good.data(), good.size()), &payload, &error));
}

void TestLayoutHashSeparatesShapes() {
  const std::string_view names[2] = {"x", "y"};
  const u32 types[2] = {4, 4};
  const u32 offsets[2] = {0, 4};
  const u32 moved[2] = {0, 8};

  const u64 base_hash = HashComponentLayout("Health", 8, names, types, offsets);
  CHECK(base_hash == HashComponentLayout("Health", 8, names, types, offsets));
  CHECK(base_hash != HashComponentLayout("Armor", 8, names, types, offsets));
  CHECK(base_hash != HashComponentLayout("Health", 12, names, types, offsets));
  CHECK(base_hash != HashComponentLayout("Health", 8, names, types, moved));
  const u32 retyped[2] = {4, 5};
  CHECK(base_hash != HashComponentLayout("Health", 8, names, retyped, offsets));
  const std::string_view renamed[2] = {"x", "z"};
  CHECK(base_hash != HashComponentLayout("Health", 8, renamed, types, offsets));
  // A prefix of the fields is not the same shape as all of them.
  CHECK(base_hash != HashComponentLayout("Health", 8, std::span(names).first(1),
                                         std::span(types).first(1), std::span(offsets).first(1)));
}

}  // namespace

int main() {
  TestPayloadPathIsDerived();
  TestIndexRoundTrip();
  TestStableIdLookupIgnoresCellOrder();
  TestIndexRefusesInconsistentWorlds();
  TestIndexRefusesCorruptedBytes();
  TestEntityPayloadRoundTrip();
  TestInstancePayloadRoundTrip();
  TestPayloadRefusesInconsistentContent();
  TestPayloadRefusesCorruptedBytes();
  TestLayoutHashSeparatesShapes();
  if (g_failures) {
    std::fprintf(stderr, "world_format_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("world_format_test: ok");
  return 0;
}
