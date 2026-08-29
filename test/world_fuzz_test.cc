// Random-mutation fuzz over the three baked-map decoders.
//
// The hand-written refusal tests in world_format_test and world_overlay_test
// each aim one mutation at one check. This aims a great many at no check in
// particular, which is the half they cannot cover: a decoder has to be safe on
// input nobody thought of, not only on input somebody thought of.
//
// Half the runs repair the checksum after mutating. Without that the checksum
// rejects essentially everything and the fuzz never reaches the structural
// validation behind it - which is where the interesting bugs are, because that
// is the code that computes offsets and counts from the file.
//
// The contract under test is narrow and absolute: a decoder may accept or it
// may refuse with a message, and it may not crash, hang, leak, or read out of
// bounds. Anything it accepts must survive its own accessors. Run under
// -DRX_SANITIZE=ON for that to mean what it says.
//
// Deterministic: fixed seed, fixed iteration count. Pass a count to run longer.
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

#include "world/world_format.h"
#include "world/world_overlay.h"

namespace {

using namespace rx::world;
using rx::f32;
using rx::u32;
using rx::u64;
using rx::u8;

struct Position {
  f32 x = 0, y = 0, z = 0;
};

constexpr u64 kBakeId = 0xabcull;

base::Vector<u8> SeedIndex() {
  WorldIndexWriter writer;
  writer.set_bake_id(kBakeId);
  writer.set_grid(64, {0, 0, 0});
  writer.AddCell(1, {}, {64, 32, 64}, 0, 0, 8);
  writer.AddCell(2, {64, 0, 0}, {128, 32, 64}, 3, 8, 8);
  writer.AddPayload(1, Domain::kGameplay, Tier::kStandard, 512, 8);
  writer.AddPayload(1, Domain::kGameplay, Tier::kProxy, 64, 2);
  writer.AddPayload(2, Domain::kRepresentation, Tier::kFull, 256, 4);
  base::Vector<u8> bytes;
  std::string error;
  writer.Encode(&bytes, &error);
  return bytes;
}

base::Vector<u8> SeedEntities() {
  const Position positions[3] = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}};
  const u64 ids[3] = {0, 1, 2};
  CellPayloadWriter writer(1, Domain::kGameplay, Tier::kStandard);
  writer.set_bake_id(kBakeId);
  const u32 archetype = writer.BeginArchetype(3);
  writer.AddColumn(archetype, "Position", sizeof(Position),
                   HashComponentLayout("Position", sizeof(Position), {}, {}, {}),
                   std::span<const u8>(reinterpret_cast<const u8*>(positions), sizeof(positions)));
  writer.SetStableIds(archetype, std::span<const u64>(ids, 3));
  base::Vector<u8> bytes;
  std::string error;
  writer.Encode(&bytes, &error);
  return bytes;
}

base::Vector<u8> SeedInstances() {
  CellPayloadWriter writer(2, Domain::kRepresentation, Tier::kFull);
  writer.set_bake_id(kBakeId);
  const u32 prototype = writer.AddPrototype("prop/rock");
  for (u32 i = 0; i < 4; ++i) {
    writer.AddInstance(8 + i, prototype, {static_cast<f32>(i), 0, 0}, {0, 0, 0, 1}, 1.0f);
  }
  base::Vector<u8> bytes;
  std::string error;
  writer.Encode(&bytes, &error);
  return bytes;
}

base::Vector<u8> SeedOverlay() {
  WorldOverlay overlay;
  overlay.set_bake_id(kBakeId);
  overlay.Destroy(3);
  overlay.Destroy(9);
  overlay.Move(1, {1, 2, 3}, {0, 0, 0, 1}, 2.0f);
  base::Vector<u8> bytes;
  std::string error;
  overlay.Encode(&bytes, &error);
  return bytes;
}

// Header sizes, so a mutation can be followed by a repaired checksum.
size_t HeaderBytes(u32 seed) { return seed == 0 ? 64 : (seed == 3 ? 32 : 72); }

void RepairChecksum(base::Vector<u8>* bytes, size_t header) {
  if (bytes->size() < header) return;
  u64 hash = 0xcbf29ce484222325ull;
  auto fold = [&](size_t first, size_t last) {
    for (size_t i = first; i < last; ++i) {
      hash ^= (*bytes)[i];
      hash *= 0x100000001b3ull;
    }
  };
  fold(0, header - sizeof(u64));
  fold(header, bytes->size());
  for (u32 shift = 0; shift < 64; shift += 8) {
    (*bytes)[header - sizeof(u64) + shift / 8] = static_cast<u8>(hash >> shift);
  }
}

}  // namespace

int main(int argc, char** argv) {
  const rx::u64 iterations = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 200000;
  const base::Vector<rx::u8> seeds[4] = {SeedIndex(), SeedEntities(), SeedInstances(), SeedOverlay()};
  for (const base::Vector<rx::u8>& seed : seeds) {
    if (!seed.empty()) continue;
    std::fprintf(stderr, "world_fuzz_test: a seed failed to encode\n");
    return 1;
  }

  std::mt19937_64 rng(0x9e3779b97f4a7c15ull);
  rx::u64 accepted = 0;
  rx::u64 refused = 0;
  for (rx::u64 i = 0; i < iterations; ++i) {
    const rx::u32 which = static_cast<rx::u32>(rng() % 4);
    base::Vector<rx::u8> bytes(seeds[which]);

    const rx::u32 edits = 1 + static_cast<rx::u32>(rng() % 8);
    for (rx::u32 edit = 0; edit < edits && !bytes.empty(); ++edit) {
      bytes[rng() % bytes.size()] = static_cast<rx::u8>(rng());
    }
    if ((rng() % 8) == 0 && bytes.size() > 1) {
      const size_t keep = rng() % bytes.size();
      while (bytes.size() > keep) bytes.erase(bytes.end() - 1);
    }
    if ((rng() % 2) == 0) RepairChecksum(&bytes, HeaderBytes(which));

    const std::span<const rx::u8> span(bytes.data(), bytes.size());
    std::string error;
    bool ok = false;
    if (which == 0) {
      WorldIndexData index;
      ok = DecodeWorldIndex(span, &index, &error);
      if (ok) {
        for (const WorldCellRecord& cell : index.cells) {
          (void)index.FindCell(cell.id);
          (void)index.FindCellByStableId(cell.stable_id_first);
          (void)index.BestTier(cell, Domain::kGameplay, Tier::kFull);
        }
      }
    } else if (which == 3) {
      WorldOverlay overlay;
      ok = WorldOverlay::Decode(span, &overlay, &error);
      if (ok) {
        for (rx::u64 id : overlay.destroyed()) (void)overlay.IsDestroyed(id);
        for (const OverlayMove& move : overlay.moves()) (void)overlay.FindMove(move.stable_id);
      }
    } else {
      WorldCellPayload payload;
      ok = DecodeCellPayload(span, &payload, &error);
      if (ok) {
        // Anything accepted has to survive its own accessors: these are what
        // the streamer calls, and they trust what the decoder validated.
        for (const WorldArchetypeRecord& archetype : payload.archetypes) {
          (void)payload.StableIds(archetype);
        }
        for (const WorldColumnRecord& column : payload.columns) {
          (void)payload.ColumnBytes(column);
          (void)payload.String(column.name);
        }
        for (const WorldPrototypeRecord& prototype : payload.prototypes) {
          (void)payload.String(prototype.name);
        }
        (void)payload.total_row_count();
      }
    }

    if (ok) {
      ++accepted;
      continue;
    }
    ++refused;
    if (!error.empty()) continue;
    std::fprintf(stderr, "world_fuzz_test: iteration %llu refused with no message\n",
                 static_cast<unsigned long long>(i));
    return 1;
  }

  // Both outcomes have to occur, or the fuzz proved nothing: all-refused would
  // mean the mutations never got past the checksum, all-accepted that the
  // decoder validates nothing.
  if (accepted == 0 || refused == 0) {
    std::fprintf(stderr, "world_fuzz_test: %llu accepted, %llu refused - one-sided\n",
                 static_cast<unsigned long long>(accepted),
                 static_cast<unsigned long long>(refused));
    return 1;
  }
  std::printf("world_fuzz_test: ok (%llu iterations, %llu accepted, %llu refused)\n",
              static_cast<unsigned long long>(iterations),
              static_cast<unsigned long long>(accepted),
              static_cast<unsigned long long>(refused));
  return 0;
}
