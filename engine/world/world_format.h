#ifndef RX_WORLD_WORLD_FORMAT_H_
#define RX_WORLD_WORLD_FORMAT_H_

#include <span>
#include <string>
#include <string_view>

#include <base/containers/vector.h>

#include "core/export.h"
#include "core/math.h"
#include "core/types.h"

// The baked map formats. Two files, two jobs:
//
//   RXWORLDI  the world index. One per world, always resident, small enough to
//             keep resident forever. It is the only thing a streaming decision
//             may read: bounds, zone, which domains and tiers exist, and what
//             each costs once resident. Opening a cell payload merely to learn
//             where the cell is, or how big it is, is the failure this file
//             exists to prevent.
//   RXCELLPL  one cell payload: one domain of one cell at one tier. Immutable
//             and cooked. Entity payloads are archetype-major component
//             columns; instance payloads are packed transforms for static
//             decoration that has no ECS identity until something needs it to.
//
// Both are little-endian, hand-rolled in the style of terrain_io.cc, and both
// refuse to load rather than substitute a default. A truncated, reordered or
// stale file is a cook bug, and a silently half-loaded world costs far more to
// diagnose than a load that names the byte it choked on.
//
// The index does not store payload paths or compressed sizes. Paths follow one
// convention (CellPayloadPath), and the archive's own table of contents is
// authoritative for on-disk bytes; duplicating either into the index only
// creates two answers that can disagree.

namespace rx::world {

// What a cell can supply, independently of every other domain. Geometry and
// texture detail are deliberately absent: those are resource-page residency
// owned by the virtual geometry and texture streamers, and tying them to a
// world cell is exactly the over-wide chunk this format exists to avoid.
// Only two payload shapes exist so far (PayloadKind below): entity columns and
// instance pages. Collision, navigation, lighting and audio are named here
// because their residency is genuinely independent and the streamer already
// schedules them separately, but nothing streams for them until content shaped
// like a collision tile or a navigation tile exists to bake.
enum class Domain : u8 {
  kGameplay = 0,        // ECS entities that need behavior
  kRepresentation = 1,  // static instance pages: decoration with no ECS identity
  kCollision = 2,       // static collision tiles
  kNavigation = 3,      // navigation tiles
  kLighting = 4,        // light and fog lists
  kAudio = 5,           // emitters and portal data
};
constexpr u32 kDomainCount = 6;

RX_WORLD_EXPORT const char* DomainName(Domain domain);

// One ladder shared by every domain, meaning "how much of this domain", not
// "which mesh LOD": kProxy is an HLOD or a coarse navigation graph, kStandard
// the normal working set, kFull everything the cook produced. Tiers are
// alternatives, not increments: exactly one is resident per domain at a time.
enum class Tier : u8 {
  kAbsent = 0,
  kProxy = 1,
  kStandard = 2,
  kFull = 3,
};
constexpr u32 kTierCount = 4;

RX_WORLD_EXPORT const char* TierName(Tier tier);

// Where one (domain, tier) of one cell lives, and what it costs resident. The
// path is not stored: it is this, and only this.
//   <prefix>/<cell id, 16 hex digits>.<domain>.<tier>.rxcell
RX_WORLD_EXPORT std::string CellPayloadPath(std::string_view prefix, u64 cell, Domain domain,
                                            Tier tier);

struct WorldPayloadRecord {
  u64 resident_bytes = 0;  // what this costs once decoded, for the memory budget
  u32 row_count = 0;       // entities or instances, for the activation budget
  Domain domain = Domain::kGameplay;
  Tier tier = Tier::kAbsent;
};

// A cell's whole manifest. The payload span points into the index's flat
// payload table so the manifest itself stays a fixed-size record.
struct WorldCellRecord {
  u64 id = 0;
  Vec3 minimum;
  Vec3 maximum;
  u32 zone = 0;
  u32 flags = 0;
  // Every stable id the cook assigned inside this cell. Ranges never overlap
  // between cells, which is what lets a stable id be resolved to its owning
  // cell by binary search without any cell being resident.
  //
  // A stable id is a streaming key, not yet a persistence key. It is assigned
  // by cook order within a cell, so re-baking a changed scene - or moving one
  // object across a cell boundary - can reassign it, and an overlay keyed to
  // the old bake no longer means what it said. That is what the bake id catches
  // on the index; a save that must survive re-cooking needs an authored
  // identity the cook maps to an id, which nothing here provides yet.
  u64 stable_id_first = 0;
  u32 stable_id_count = 0;
  u32 payload_first = 0;
  u32 payload_count = 0;
};

// The decoded index. Cells are sorted by id, so lookup is a binary search and
// iteration order never depends on cook order.
struct WorldIndexData {
  u32 version = 0;
  u64 world_id = 0;
  // Identifies the cook that produced this index. Every payload carries the
  // same value; a mismatch at load means the index and the archive came from
  // different bakes, and is refused rather than materialized.
  u64 bake_id = 0;
  f32 cell_size = 0;  // 0 when the world is not on a regular grid
  Vec3 grid_origin;
  base::Vector<WorldCellRecord> cells;
  base::Vector<WorldPayloadRecord> payloads;

  const WorldCellRecord* FindCell(u64 id) const;
  // The cell whose stable-id range contains `stable_id`, or null.
  const WorldCellRecord* FindCellByStableId(u64 stable_id) const;
  const WorldPayloadRecord* FindPayload(const WorldCellRecord& cell, Domain domain,
                                        Tier tier) const;
  // The best tier the cook produced for this domain at or below `ceiling`.
  Tier BestTier(const WorldCellRecord& cell, Domain domain, Tier ceiling) const;
};

RX_WORLD_EXPORT bool DecodeWorldIndex(std::span<const u8> bytes, WorldIndexData* out,
                                      std::string* error);

// Cook side. Cells and payloads arrive in any order; Encode sorts and flattens.
// Re-adding the same cell, or the same (cell, domain, tier), replaces the
// earlier record, so re-cooking one domain does not need the whole world.
class RX_WORLD_EXPORT WorldIndexWriter {
 public:
  void set_world_id(u64 id) { world_id_ = id; }
  void set_bake_id(u64 id) { bake_id_ = id; }
  void set_grid(f32 cell_size, Vec3 origin);

  // Bounds are canonicalized (min and max swapped when inverted).
  void AddCell(u64 id, Vec3 minimum, Vec3 maximum, u32 zone, u64 stable_id_first,
               u32 stable_id_count);
  void SetCellFlags(u64 id, u32 flags);
  void AddPayload(u64 cell, Domain domain, Tier tier, u64 resident_bytes, u32 row_count);

  size_t cell_count() const { return cells_.size(); }

  // False, with `error` set, when the world is inconsistent: a payload naming a
  // cell that was never added, a payload baked at tier absent, a duplicate cell
  // id, or two cells whose stable-id ranges overlap. Catching an overlapping
  // range here is the whole point: at runtime it would resolve a stable id to
  // the wrong cell, silently, forever.
  bool Encode(base::Vector<u8>* out, std::string* error) const;

 private:
  struct PendingCell {
    u64 id = 0;
    Vec3 minimum;
    Vec3 maximum;
    u32 zone = 0;
    u32 flags = 0;
    u64 stable_id_first = 0;
    u32 stable_id_count = 0;
  };
  struct PendingPayload {
    u64 cell = 0;
    u64 resident_bytes = 0;
    u32 row_count = 0;
    Domain domain = Domain::kGameplay;
    Tier tier = Tier::kAbsent;
  };

  PendingCell* Find(u64 id);

  base::Vector<PendingCell> cells_;
  base::Vector<PendingPayload> payloads_;
  u64 world_id_ = 0;
  u64 bake_id_ = 0;
  f32 cell_size_ = 0;
  Vec3 grid_origin_;
};

// ---------------------------------------------------------------------------
// Cell payloads

enum class PayloadKind : u8 {
  kEntities = 0,   // archetype-major component columns
  kInstances = 1,  // packed static decoration, promoted to ECS only on demand
};

// One component column of one archetype. Components are named, never numbered:
// ecs::ComponentId is assigned in first-use order at runtime and differs
// between runs, so it can never reach disk. `layout_hash` is the cook's record
// of the component's reflected field layout; the loader recomputes it from the
// running build and refuses the payload when they differ, because column bytes
// are the struct verbatim and a silent ABI drift would corrupt every entity in
// the cell instead of failing.
struct WorldColumnRecord {
  u32 name = 0;  // string-table offset of the reflected component name
  u32 stride = 0;
  u64 layout_hash = 0;
  u64 data_offset = 0;  // from the start of the payload's data section
  u64 data_bytes = 0;
};

struct WorldArchetypeRecord {
  u32 row_count = 0;
  u32 column_first = 0;
  u32 column_count = 0;
  u64 stable_id_offset = 0;  // u64[row_count], from the start of the data section
};

struct WorldPrototypeRecord {
  u32 name = 0;  // string-table offset
};

// A static decoration instance: no ECS identity, but a stable world id, so it
// can be picked, deleted by an overlay, or promoted into an entity when its
// behavior finally matters.
struct WorldInstanceRecord {
  u64 stable_id = 0;
  u32 prototype = 0;  // index into the prototype table
  Vec3 position;
  Quat rotation;
  f32 scale = 1.0f;
};

struct WorldCellPayload {
  u32 version = 0;
  PayloadKind kind = PayloadKind::kEntities;
  u64 cell_id = 0;
  u64 bake_id = 0;
  Domain domain = Domain::kGameplay;
  Tier tier = Tier::kAbsent;
  base::Vector<WorldArchetypeRecord> archetypes;
  base::Vector<WorldColumnRecord> columns;
  base::Vector<WorldPrototypeRecord> prototypes;
  base::Vector<WorldInstanceRecord> instances;
  base::Vector<char> strings;
  base::Vector<u8> data;  // column bytes and stable-id arrays

  std::string_view String(u32 offset) const;
  std::span<const u8> ColumnBytes(const WorldColumnRecord& column) const;
  std::span<const u64> StableIds(const WorldArchetypeRecord& archetype) const;
  u32 total_row_count() const;
};

RX_WORLD_EXPORT bool DecodeCellPayload(std::span<const u8> bytes, WorldCellPayload* out,
                                       std::string* error);

// Cook side for one payload. The writer owns the data section, so callers never
// compute an offset.
class RX_WORLD_EXPORT CellPayloadWriter {
 public:
  CellPayloadWriter(u64 cell_id, Domain domain, Tier tier);

  void set_bake_id(u64 id) { bake_id_ = id; }

  // Entity payloads: open an archetype, add one column per component holding
  // the full row_count worth of bytes, then close it with the stable ids.
  u32 BeginArchetype(u32 row_count);
  void AddColumn(u32 archetype, std::string_view component, u32 stride, u64 layout_hash,
                 std::span<const u8> bytes);
  void SetStableIds(u32 archetype, std::span<const u64> ids);

  // Instance payloads.
  u32 AddPrototype(std::string_view name);
  void AddInstance(u64 stable_id, u32 prototype, Vec3 position, Quat rotation, f32 scale);

  // False, with `error` set, when the payload is internally inconsistent: a
  // column whose bytes are not stride * row_count, an archetype missing stable
  // ids, a duplicate component in one archetype, a duplicate stable id, or
  // entity and instance content mixed into one payload.
  bool Encode(base::Vector<u8>* out, std::string* error) const;

 private:
  struct PendingColumn {
    u32 archetype = 0;
    std::string component;
    u32 stride = 0;
    u64 layout_hash = 0;
    base::Vector<u8> bytes;
  };
  struct PendingArchetype {
    u32 row_count = 0;
    base::Vector<u64> stable_ids;
  };

  u64 cell_id_ = 0;
  u64 bake_id_ = 0;
  Domain domain_ = Domain::kGameplay;
  Tier tier_ = Tier::kAbsent;
  base::Vector<PendingArchetype> archetypes_;
  base::Vector<PendingColumn> columns_;
  base::Vector<std::string> prototypes_;
  base::Vector<WorldInstanceRecord> instances_;
};

// fnv1a-64 over the reflected shape of a component: its name, its stride, and
// every field's name, type and offset. Two builds that agree on this agree on
// the bytes of the struct, which is what makes a column safe to memcpy.
RX_WORLD_EXPORT u64 HashComponentLayout(std::string_view component, u32 stride,
                                        std::span<const std::string_view> field_names,
                                        std::span<const u32> field_types,
                                        std::span<const u32> field_offsets);

}  // namespace rx::world

#endif  // RX_WORLD_WORLD_FORMAT_H_
