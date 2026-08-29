#include "world/world_format.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace rx::world {
namespace {

constexpr u8 kIndexMagic[8] = {'R', 'X', 'W', 'O', 'R', 'L', 'D', 'I'};
constexpr u8 kPayloadMagic[8] = {'R', 'X', 'C', 'E', 'L', 'L', 'P', 'L'};
constexpr u32 kIndexVersion = 1;
constexpr u32 kPayloadVersion = 1;

// Ceilings on anything a corrupt header could ask us to allocate. They sit far
// above any real cook and exist only so a truncated file fails on its count
// rather than on the reserve that count drives.
constexpr u32 kMaximumCells = 4'000'000;
constexpr u32 kMaximumPayloads = 32'000'000;
constexpr u32 kMaximumArchetypes = 65'536;
constexpr u32 kMaximumColumns = 1'000'000;
constexpr u32 kMaximumPrototypes = 1'000'000;
constexpr u32 kMaximumInstances = 16'000'000;
constexpr u32 kMaximumStringBytes = 64u * 1024 * 1024;
constexpr u64 kMaximumDataBytes = 2ull * 1024 * 1024 * 1024;

constexpr u32 kIndexCellBytes = 60;
constexpr u32 kIndexPayloadBytes = 16;
constexpr u32 kArchetypeBytes = 20;
constexpr u32 kColumnBytes = 32;
constexpr u32 kPrototypeBytes = 4;
constexpr u32 kInstanceBytes = 44;

void SetError(std::string* error, std::string message) {
  if (error) *error = std::move(message);
}

u64 Checksum(std::span<const u8> bytes) {
  u64 hash = 0xcbf29ce484222325ull;
  for (u8 byte : bytes) {
    hash ^= byte;
    hash *= 0x100000001b3ull;
  }
  return hash;
}

u64 HashBytes(u64 hash, const void* data, size_t size) {
  const u8* bytes = static_cast<const u8*>(data);
  for (size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 0x100000001b3ull;
  }
  return hash;
}

void AppendU8(base::Vector<u8>* bytes, u8 value) { bytes->push_back(value); }

void AppendU16(base::Vector<u8>* bytes, u16 value) {
  AppendU8(bytes, static_cast<u8>(value));
  AppendU8(bytes, static_cast<u8>(value >> 8));
}

void AppendU32(base::Vector<u8>* bytes, u32 value) {
  for (u32 shift = 0; shift < 32; shift += 8) AppendU8(bytes, static_cast<u8>(value >> shift));
}

void AppendU64(base::Vector<u8>* bytes, u64 value) {
  for (u32 shift = 0; shift < 64; shift += 8) AppendU8(bytes, static_cast<u8>(value >> shift));
}

void AppendF32(base::Vector<u8>* bytes, f32 value) { AppendU32(bytes, std::bit_cast<u32>(value)); }

void AppendVec3(base::Vector<u8>* bytes, const Vec3& value) {
  AppendF32(bytes, value.x);
  AppendF32(bytes, value.y);
  AppendF32(bytes, value.z);
}

// Sequential little-endian reader. Every read is bounds checked, and once `ok`
// goes false it stays false, so a caller may read a whole record and test once.
class Cursor {
 public:
  explicit Cursor(std::span<const u8> bytes) : bytes_(bytes) {}

  bool ok() const { return ok_; }
  size_t offset() const { return offset_; }

  bool Take(size_t count, const u8** out) {
    if (!ok_ || bytes_.size() - offset_ < count) {
      ok_ = false;
      return false;
    }
    *out = bytes_.data() + offset_;
    offset_ += count;
    return true;
  }

  u8 U8() {
    const u8* data = nullptr;
    if (!Take(1, &data)) return 0;
    return *data;
  }

  u16 U16() {
    const u16 low = U8();
    return static_cast<u16>(low | (static_cast<u16>(U8()) << 8));
  }

  u32 U32() {
    u32 value = 0;
    for (u32 shift = 0; shift < 32; shift += 8) value |= static_cast<u32>(U8()) << shift;
    return value;
  }

  u64 U64() {
    u64 value = 0;
    for (u32 shift = 0; shift < 64; shift += 8) value |= static_cast<u64>(U8()) << shift;
    return value;
  }

  f32 F32() { return std::bit_cast<f32>(U32()); }

  Vec3 ReadVec3() {
    Vec3 value;
    value.x = F32();
    value.y = F32();
    value.z = F32();
    return value;
  }

 private:
  std::span<const u8> bytes_;
  size_t offset_ = 0;
  bool ok_ = true;
};

// Cook-time string interner. Offset 0 is always the empty string, so a record
// that names nothing needs no special case on the read side. A world's tables
// hold component and prototype names - tens of entries, not thousands - so a
// linear scan is cheaper than the hash map it would replace.
class StringTable {
 public:
  StringTable() { bytes_.push_back('\0'); }

  u32 Intern(std::string_view value) {
    if (value.empty()) return 0;
    for (u32 offset : offsets_) {
      const char* existing = bytes_.data() + offset;
      if (value.size() == std::char_traits<char>::length(existing) &&
          std::memcmp(existing, value.data(), value.size()) == 0) {
        return offset;
      }
    }
    const u32 offset = static_cast<u32>(bytes_.size());
    bytes_.insert(bytes_.end(), value.begin(), value.end());
    bytes_.push_back('\0');
    offsets_.push_back(offset);
    return offset;
  }

  const base::Vector<char>& bytes() const { return bytes_; }

 private:
  base::Vector<char> bytes_;
  base::Vector<u32> offsets_;
};

std::string_view StringAt(const base::Vector<char>& table, u32 offset) {
  if (offset >= table.size()) return {};
  const char* begin = table.data() + offset;
  const size_t limit = table.size() - offset;
  const size_t length = std::char_traits<char>::length(begin);
  return length < limit ? std::string_view(begin, length) : std::string_view();
}

// A string table is only usable if its last byte terminates; otherwise a view
// built from its tail would run past the buffer.
bool StringTableTerminated(const base::Vector<char>& table) {
  return !table.empty() && table.back() == '\0';
}

bool IsFinite(const Vec3& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool ValidDomain(u8 value) { return value < kDomainCount; }
bool ValidTier(u8 value) { return value < kTierCount; }

void AppendHex64(std::string* out, u64 value) {
  static const char kDigits[] = "0123456789abcdef";
  for (int shift = 60; shift >= 0; shift -= 4) out->push_back(kDigits[(value >> shift) & 0xf]);
}

}  // namespace

const char* DomainName(Domain domain) {
  switch (domain) {
    case Domain::kGameplay: return "gameplay";
    case Domain::kRepresentation: return "representation";
    case Domain::kCollision: return "collision";
    case Domain::kNavigation: return "navigation";
    case Domain::kLighting: return "lighting";
    case Domain::kAudio: return "audio";
  }
  return "unknown";
}

const char* TierName(Tier tier) {
  switch (tier) {
    case Tier::kAbsent: return "absent";
    case Tier::kProxy: return "proxy";
    case Tier::kStandard: return "standard";
    case Tier::kFull: return "full";
  }
  return "unknown";
}

std::string CellPayloadPath(std::string_view prefix, u64 cell, Domain domain, Tier tier) {
  std::string path(prefix);
  if (!path.empty() && path.back() != '/') path.push_back('/');
  AppendHex64(&path, cell);
  path.push_back('.');
  path.append(DomainName(domain));
  path.push_back('.');
  path.append(TierName(tier));
  path.append(".rxcell");
  return path;
}

u64 HashComponentLayout(std::string_view component, u32 stride,
                        std::span<const std::string_view> field_names,
                        std::span<const u32> field_types, std::span<const u32> field_offsets) {
  u64 hash = 0xcbf29ce484222325ull;
  hash = HashBytes(hash, component.data(), component.size());
  hash = HashBytes(hash, &stride, sizeof(stride));
  const size_t count =
      std::min(field_names.size(), std::min(field_types.size(), field_offsets.size()));
  const u32 field_count = static_cast<u32>(count);
  hash = HashBytes(hash, &field_count, sizeof(field_count));
  for (size_t i = 0; i < count; ++i) {
    hash = HashBytes(hash, field_names[i].data(), field_names[i].size());
    hash = HashBytes(hash, &field_types[i], sizeof(u32));
    hash = HashBytes(hash, &field_offsets[i], sizeof(u32));
  }
  return hash;
}

const WorldCellRecord* WorldIndexData::FindCell(u64 id) const {
  auto it = std::lower_bound(cells.begin(), cells.end(), id,
                             [](const WorldCellRecord& cell, u64 wanted) { return cell.id < wanted; });
  return it != cells.end() && it->id == id ? it : nullptr;
}

const WorldCellRecord* WorldIndexData::FindCellByStableId(u64 stable_id) const {
  // Ranges never overlap (the writer refuses a world where they do), so the
  // last cell whose range starts at or below the id is the only candidate.
  auto it = std::upper_bound(cells.begin(), cells.end(), stable_id,
                             [](u64 wanted, const WorldCellRecord& cell) {
                               return wanted < cell.stable_id_first;
                             });
  if (it == cells.begin()) return nullptr;
  --it;
  return stable_id - it->stable_id_first < it->stable_id_count ? it : nullptr;
}

const WorldPayloadRecord* WorldIndexData::FindPayload(const WorldCellRecord& cell, Domain domain,
                                                      Tier tier) const {
  for (u32 i = 0; i < cell.payload_count; ++i) {
    const WorldPayloadRecord& payload = payloads[cell.payload_first + i];
    if (payload.domain == domain && payload.tier == tier) return &payload;
  }
  return nullptr;
}

Tier WorldIndexData::BestTier(const WorldCellRecord& cell, Domain domain, Tier ceiling) const {
  Tier best = Tier::kAbsent;
  for (u32 i = 0; i < cell.payload_count; ++i) {
    const WorldPayloadRecord& payload = payloads[cell.payload_first + i];
    if (payload.domain != domain || payload.tier > ceiling) continue;
    if (payload.tier > best) best = payload.tier;
  }
  return best;
}

// ---------------------------------------------------------------------------
// Index

void WorldIndexWriter::set_grid(f32 cell_size, Vec3 origin) {
  cell_size_ = cell_size;
  grid_origin_ = origin;
}

WorldIndexWriter::PendingCell* WorldIndexWriter::Find(u64 id) {
  for (PendingCell& cell : cells_) {
    if (cell.id == id) return &cell;
  }
  return nullptr;
}

void WorldIndexWriter::AddCell(u64 id, Vec3 minimum, Vec3 maximum, u32 zone, u64 stable_id_first,
                               u32 stable_id_count) {
  const Vec3 low{std::min(minimum.x, maximum.x), std::min(minimum.y, maximum.y),
                 std::min(minimum.z, maximum.z)};
  const Vec3 high{std::max(minimum.x, maximum.x), std::max(minimum.y, maximum.y),
                  std::max(minimum.z, maximum.z)};
  if (PendingCell* existing = Find(id)) {
    existing->minimum = low;
    existing->maximum = high;
    existing->zone = zone;
    existing->stable_id_first = stable_id_first;
    existing->stable_id_count = stable_id_count;
    return;
  }
  cells_.push_back({id, low, high, zone, 0, stable_id_first, stable_id_count});
}

void WorldIndexWriter::SetCellFlags(u64 id, u32 flags) {
  if (PendingCell* existing = Find(id)) existing->flags = flags;
}

void WorldIndexWriter::AddPayload(u64 cell, Domain domain, Tier tier, u64 resident_bytes,
                                  u32 row_count) {
  for (PendingPayload& payload : payloads_) {
    if (payload.cell != cell || payload.domain != domain || payload.tier != tier) continue;
    payload.resident_bytes = resident_bytes;
    payload.row_count = row_count;
    return;
  }
  payloads_.push_back({cell, resident_bytes, row_count, domain, tier});
}

bool WorldIndexWriter::Encode(base::Vector<u8>* out, std::string* error) const {
  if (!out) return false;
  if (cells_.size() > kMaximumCells) {
    SetError(error, "world index: too many cells");
    return false;
  }
  if (payloads_.size() > kMaximumPayloads) {
    SetError(error, "world index: too many payloads");
    return false;
  }

  base::Vector<PendingCell> cells(cells_);
  std::sort(cells.begin(), cells.end(),
            [](const PendingCell& a, const PendingCell& b) { return a.id < b.id; });
  for (size_t i = 1; i < cells.size(); ++i) {
    if (cells[i].id == cells[i - 1].id) {
      SetError(error, "world index: duplicate cell id " + std::to_string(cells[i].id));
      return false;
    }
  }

  // Overlapping stable-id ranges would resolve an id to the wrong cell at
  // runtime, without ever failing. Sorting by range start makes the check one
  // pass; cells that own nothing are skipped rather than compared.
  base::Vector<const PendingCell*> ranged;
  ranged.reserve(cells.size());
  for (const PendingCell& cell : cells) {
    if (cell.stable_id_count != 0) ranged.push_back(&cell);
  }
  std::sort(ranged.begin(), ranged.end(), [](const PendingCell* a, const PendingCell* b) {
    if (a->stable_id_first != b->stable_id_first) return a->stable_id_first < b->stable_id_first;
    return a->id < b->id;
  });
  for (const PendingCell* cell : ranged) {
    if (cell->stable_id_first > ~u64{0} - cell->stable_id_count) {
      SetError(error, "world index: cell " + std::to_string(cell->id) +
                          " stable-id range wraps past the end of the id space");
      return false;
    }
  }
  for (size_t i = 1; i < ranged.size(); ++i) {
    const PendingCell& previous = *ranged[i - 1];
    const PendingCell& current = *ranged[i];
    if (previous.stable_id_first + previous.stable_id_count > current.stable_id_first) {
      SetError(error, "world index: cells " + std::to_string(previous.id) + " and " +
                          std::to_string(current.id) + " have overlapping stable-id ranges");
      return false;
    }
  }

  auto known_cell = [&](u64 id) {
    auto it =
        std::lower_bound(cells.begin(), cells.end(), id,
                         [](const PendingCell& cell, u64 wanted) { return cell.id < wanted; });
    return it != cells.end() && it->id == id;
  };

  base::Vector<PendingPayload> payloads(payloads_);
  for (const PendingPayload& payload : payloads) {
    if (!known_cell(payload.cell)) {
      SetError(error, "world index: payload names unknown cell " + std::to_string(payload.cell));
      return false;
    }
    if (payload.tier == Tier::kAbsent) {
      SetError(error, "world index: cell " + std::to_string(payload.cell) + " " +
                          DomainName(payload.domain) + " payload is baked at tier absent");
      return false;
    }
  }
  std::sort(payloads.begin(), payloads.end(), [](const PendingPayload& a, const PendingPayload& b) {
    if (a.cell != b.cell) return a.cell < b.cell;
    if (a.domain != b.domain) return a.domain < b.domain;
    return a.tier < b.tier;
  });

  base::Vector<u8> body;
  size_t payload_cursor = 0;
  for (const PendingCell& cell : cells) {
    const u32 payload_first = static_cast<u32>(payload_cursor);
    while (payload_cursor < payloads.size() && payloads[payload_cursor].cell == cell.id) {
      ++payload_cursor;
    }
    AppendU64(&body, cell.id);
    AppendVec3(&body, cell.minimum);
    AppendVec3(&body, cell.maximum);
    AppendU32(&body, cell.zone);
    AppendU32(&body, cell.flags);
    AppendU64(&body, cell.stable_id_first);
    AppendU32(&body, cell.stable_id_count);
    AppendU32(&body, payload_first);
    AppendU32(&body, static_cast<u32>(payload_cursor) - payload_first);
  }

  for (const PendingPayload& payload : payloads) {
    AppendU64(&body, payload.resident_bytes);
    AppendU32(&body, payload.row_count);
    AppendU8(&body, static_cast<u8>(payload.domain));
    AppendU8(&body, static_cast<u8>(payload.tier));
    AppendU16(&body, 0);
  }

  out->clear();
  out->insert(out->end(), std::begin(kIndexMagic), std::end(kIndexMagic));
  AppendU32(out, kIndexVersion);
  AppendU32(out, 0);  // flags, reserved
  AppendU64(out, world_id_);
  AppendU64(out, bake_id_);
  AppendF32(out, cell_size_);
  AppendVec3(out, grid_origin_);
  AppendU32(out, static_cast<u32>(cells.size()));
  AppendU32(out, static_cast<u32>(payloads.size()));
  AppendU64(out, Checksum(std::span<const u8>(body.data(), body.size())));
  out->insert(out->end(), body.begin(), body.end());
  return true;
}

bool DecodeWorldIndex(std::span<const u8> bytes, WorldIndexData* out, std::string* error) {
  if (!out) return false;
  Cursor header(bytes);
  const u8* magic = nullptr;
  if (!header.Take(sizeof(kIndexMagic), &magic) ||
      std::memcmp(magic, kIndexMagic, sizeof(kIndexMagic)) != 0) {
    SetError(error, "world index: not an RXWORLDI file");
    return false;
  }
  const u32 version = header.U32();
  if (version != kIndexVersion) {
    SetError(error, "world index: version " + std::to_string(version) + ", expected " +
                        std::to_string(kIndexVersion));
    return false;
  }
  header.U32();  // flags, reserved
  const u64 world_id = header.U64();
  const u64 bake_id = header.U64();
  const f32 cell_size = header.F32();
  const Vec3 grid_origin = header.ReadVec3();
  const u32 cell_count = header.U32();
  const u32 payload_count = header.U32();
  const u64 checksum = header.U64();
  if (!header.ok()) {
    SetError(error, "world index: truncated header");
    return false;
  }
  if (cell_count > kMaximumCells || payload_count > kMaximumPayloads) {
    SetError(error, "world index: header counts out of range");
    return false;
  }

  const size_t body_offset = header.offset();
  const u64 expected_body = static_cast<u64>(cell_count) * kIndexCellBytes +
                            static_cast<u64>(payload_count) * kIndexPayloadBytes;
  if (bytes.size() - body_offset != expected_body) {
    SetError(error, "world index: body is " + std::to_string(bytes.size() - body_offset) +
                        " bytes, header describes " + std::to_string(expected_body));
    return false;
  }
  const std::span<const u8> body = bytes.subspan(body_offset);
  if (Checksum(body) != checksum) {
    SetError(error, "world index: checksum mismatch");
    return false;
  }

  *out = WorldIndexData{};
  out->version = version;
  out->world_id = world_id;
  out->bake_id = bake_id;
  out->cell_size = std::isfinite(cell_size) && cell_size > 0 ? cell_size : 0;
  out->grid_origin = IsFinite(grid_origin) ? grid_origin : Vec3{};

  Cursor cursor(body);
  out->cells.reserve(cell_count);
  for (u32 i = 0; i < cell_count; ++i) {
    WorldCellRecord cell;
    cell.id = cursor.U64();
    cell.minimum = cursor.ReadVec3();
    cell.maximum = cursor.ReadVec3();
    cell.zone = cursor.U32();
    cell.flags = cursor.U32();
    cell.stable_id_first = cursor.U64();
    cell.stable_id_count = cursor.U32();
    cell.payload_first = cursor.U32();
    cell.payload_count = cursor.U32();
    if (!IsFinite(cell.minimum) || !IsFinite(cell.maximum)) {
      SetError(error, "world index: cell " + std::to_string(cell.id) + " has non-finite bounds");
      return false;
    }
    if (cell.minimum.x > cell.maximum.x || cell.minimum.y > cell.maximum.y ||
        cell.minimum.z > cell.maximum.z) {
      SetError(error, "world index: cell " + std::to_string(cell.id) + " has inverted bounds");
      return false;
    }
    if (i > 0 && cell.id <= out->cells[i - 1].id) {
      SetError(error, "world index: cells are not sorted by id at " + std::to_string(i));
      return false;
    }
    if (static_cast<u64>(cell.payload_first) + cell.payload_count > payload_count) {
      SetError(error, "world index: cell " + std::to_string(cell.id) +
                          " spans past the payload table");
      return false;
    }
    if (cell.stable_id_count != 0 && cell.stable_id_first > ~u64{0} - cell.stable_id_count) {
      SetError(error, "world index: cell " + std::to_string(cell.id) +
                          " stable-id range wraps past the end of the id space");
      return false;
    }
    out->cells.push_back(cell);
  }

  // FindCellByStableId depends on non-overlapping ranges, and a hand-edited or
  // corrupted index is exactly where that would stop holding.
  base::Vector<const WorldCellRecord*> ranged;
  ranged.reserve(out->cells.size());
  for (const WorldCellRecord& cell : out->cells) {
    if (cell.stable_id_count != 0) ranged.push_back(&cell);
  }
  std::sort(ranged.begin(), ranged.end(),
            [](const WorldCellRecord* a, const WorldCellRecord* b) {
              return a->stable_id_first < b->stable_id_first;
            });
  for (size_t i = 1; i < ranged.size(); ++i) {
    if (ranged[i - 1]->stable_id_first + ranged[i - 1]->stable_id_count >
        ranged[i]->stable_id_first) {
      SetError(error, "world index: cells " + std::to_string(ranged[i - 1]->id) + " and " +
                          std::to_string(ranged[i]->id) + " have overlapping stable-id ranges");
      return false;
    }
  }

  out->payloads.reserve(payload_count);
  for (u32 i = 0; i < payload_count; ++i) {
    WorldPayloadRecord payload;
    payload.resident_bytes = cursor.U64();
    payload.row_count = cursor.U32();
    const u8 domain = cursor.U8();
    const u8 tier = cursor.U8();
    cursor.U16();
    if (!ValidDomain(domain) || !ValidTier(tier) || tier == static_cast<u8>(Tier::kAbsent)) {
      SetError(error, "world index: payload " + std::to_string(i) + " has an unknown domain/tier");
      return false;
    }
    payload.domain = static_cast<Domain>(domain);
    payload.tier = static_cast<Tier>(tier);
    out->payloads.push_back(payload);
  }
  if (!cursor.ok()) {
    SetError(error, "world index: truncated body");
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Cell payload

CellPayloadWriter::CellPayloadWriter(u64 cell_id, Domain domain, Tier tier)
    : cell_id_(cell_id), domain_(domain), tier_(tier) {}

u32 CellPayloadWriter::BeginArchetype(u32 row_count) {
  archetypes_.push_back({row_count, {}});
  return static_cast<u32>(archetypes_.size() - 1);
}

void CellPayloadWriter::AddColumn(u32 archetype, std::string_view component, u32 stride,
                                  u64 layout_hash, std::span<const u8> bytes) {
  PendingColumn column;
  column.archetype = archetype;
  column.component.assign(component);
  column.stride = stride;
  column.layout_hash = layout_hash;
  column.bytes.insert(column.bytes.end(), bytes.begin(), bytes.end());
  columns_.push_back(std::move(column));
}

void CellPayloadWriter::SetStableIds(u32 archetype, std::span<const u64> ids) {
  if (archetype >= archetypes_.size()) return;
  base::Vector<u64>& target = archetypes_[archetype].stable_ids;
  target.clear();
  target.insert(target.end(), ids.begin(), ids.end());
}

u32 CellPayloadWriter::AddPrototype(std::string_view name) {
  for (size_t i = 0; i < prototypes_.size(); ++i) {
    if (prototypes_[i] == name) return static_cast<u32>(i);
  }
  prototypes_.push_back(std::string(name));
  return static_cast<u32>(prototypes_.size() - 1);
}

void CellPayloadWriter::AddInstance(u64 stable_id, u32 prototype, Vec3 position, Quat rotation,
                                    f32 scale) {
  instances_.push_back({stable_id, prototype, position, rotation, scale});
}

bool CellPayloadWriter::Encode(base::Vector<u8>* out, std::string* error) const {
  if (!out) return false;

  const bool has_entities = !archetypes_.empty() || !columns_.empty();
  const bool has_instances = !prototypes_.empty() || !instances_.empty();
  if (has_entities && has_instances) {
    SetError(error, "cell payload: entity columns and instances cannot share one payload");
    return false;
  }
  const PayloadKind kind = has_instances ? PayloadKind::kInstances : PayloadKind::kEntities;

  if (archetypes_.size() > kMaximumArchetypes || columns_.size() > kMaximumColumns ||
      prototypes_.size() > kMaximumPrototypes || instances_.size() > kMaximumInstances) {
    SetError(error, "cell payload: table too large");
    return false;
  }

  for (const PendingColumn& column : columns_) {
    if (column.archetype >= archetypes_.size()) {
      SetError(error, "cell payload: column '" + column.component + "' names unknown archetype " +
                          std::to_string(column.archetype));
      return false;
    }
    if (column.stride == 0) {
      SetError(error, "cell payload: column '" + column.component + "' has a zero stride");
      return false;
    }
    const u64 expected = static_cast<u64>(column.stride) * archetypes_[column.archetype].row_count;
    if (column.bytes.size() != expected) {
      SetError(error, "cell payload: column '" + column.component + "' has " +
                          std::to_string(column.bytes.size()) + " bytes, expected " +
                          std::to_string(expected));
      return false;
    }
  }
  for (size_t a = 0; a < archetypes_.size(); ++a) {
    if (archetypes_[a].stable_ids.size() != archetypes_[a].row_count) {
      SetError(error, "cell payload: archetype " + std::to_string(a) + " has " +
                          std::to_string(archetypes_[a].stable_ids.size()) + " stable ids for " +
                          std::to_string(archetypes_[a].row_count) + " rows");
      return false;
    }
  }

  // Duplicate stable ids inside a payload would make the runtime map ambiguous
  // and an overlay's delete land on whichever row happened to be indexed last.
  base::Vector<u64> seen_ids;
  for (const PendingArchetype& archetype : archetypes_) {
    seen_ids.insert(seen_ids.end(), archetype.stable_ids.begin(), archetype.stable_ids.end());
  }
  for (const WorldInstanceRecord& instance : instances_) seen_ids.push_back(instance.stable_id);
  std::sort(seen_ids.begin(), seen_ids.end());
  for (size_t i = 1; i < seen_ids.size(); ++i) {
    if (seen_ids[i] == seen_ids[i - 1]) {
      SetError(error, "cell payload: stable id " + std::to_string(seen_ids[i]) + " appears twice");
      return false;
    }
  }

  for (const WorldInstanceRecord& instance : instances_) {
    if (instance.prototype >= prototypes_.size()) {
      SetError(error, "cell payload: instance " + std::to_string(instance.stable_id) +
                          " names unknown prototype " + std::to_string(instance.prototype));
      return false;
    }
  }

  // Columns are grouped by archetype so each archetype owns a contiguous span.
  base::Vector<const PendingColumn*> ordered;
  ordered.reserve(columns_.size());
  for (size_t a = 0; a < archetypes_.size(); ++a) {
    const size_t first = ordered.size();
    for (const PendingColumn& column : columns_) {
      if (column.archetype != a) continue;
      for (size_t seen = first; seen < ordered.size(); ++seen) {
        if (ordered[seen]->component == column.component) {
          SetError(error, "cell payload: archetype " + std::to_string(a) + " lists component '" +
                              column.component + "' twice");
          return false;
        }
      }
      ordered.push_back(&column);
    }
  }

  StringTable strings;
  base::Vector<u8> data;
  base::Vector<WorldArchetypeRecord> archetype_records;
  base::Vector<WorldColumnRecord> column_records;
  archetype_records.reserve(archetypes_.size());
  column_records.reserve(ordered.size());

  auto align_data = [&] {
    while ((data.size() % 8) != 0) data.push_back(0);
  };

  size_t ordered_cursor = 0;
  for (size_t a = 0; a < archetypes_.size(); ++a) {
    WorldArchetypeRecord record;
    record.row_count = archetypes_[a].row_count;
    record.column_first = static_cast<u32>(column_records.size());
    while (ordered_cursor < ordered.size() && ordered[ordered_cursor]->archetype == a) {
      const PendingColumn& column = *ordered[ordered_cursor];
      align_data();
      WorldColumnRecord column_record;
      column_record.name = strings.Intern(column.component);
      column_record.stride = column.stride;
      column_record.layout_hash = column.layout_hash;
      column_record.data_offset = data.size();
      column_record.data_bytes = column.bytes.size();
      data.insert(data.end(), column.bytes.begin(), column.bytes.end());
      column_records.push_back(column_record);
      ++ordered_cursor;
    }
    record.column_count = static_cast<u32>(column_records.size()) - record.column_first;
    align_data();
    record.stable_id_offset = data.size();
    for (u64 id : archetypes_[a].stable_ids) AppendU64(&data, id);
    archetype_records.push_back(record);
  }

  base::Vector<u32> prototype_names;
  prototype_names.reserve(prototypes_.size());
  for (const std::string& name : prototypes_) prototype_names.push_back(strings.Intern(name));

  if (strings.bytes().size() > kMaximumStringBytes) {
    SetError(error, "cell payload: string table too large");
    return false;
  }
  if (data.size() > kMaximumDataBytes) {
    SetError(error, "cell payload: data section too large");
    return false;
  }

  base::Vector<u8> body;
  for (const WorldArchetypeRecord& record : archetype_records) {
    AppendU32(&body, record.row_count);
    AppendU32(&body, record.column_first);
    AppendU32(&body, record.column_count);
    AppendU64(&body, record.stable_id_offset);
  }
  for (const WorldColumnRecord& record : column_records) {
    AppendU32(&body, record.name);
    AppendU32(&body, record.stride);
    AppendU64(&body, record.layout_hash);
    AppendU64(&body, record.data_offset);
    AppendU64(&body, record.data_bytes);
  }
  for (u32 name : prototype_names) AppendU32(&body, name);
  for (const WorldInstanceRecord& instance : instances_) {
    AppendU64(&body, instance.stable_id);
    AppendU32(&body, instance.prototype);
    AppendVec3(&body, instance.position);
    AppendF32(&body, instance.rotation.x);
    AppendF32(&body, instance.rotation.y);
    AppendF32(&body, instance.rotation.z);
    AppendF32(&body, instance.rotation.w);
    AppendF32(&body, instance.scale);
  }
  body.insert(body.end(), strings.bytes().begin(), strings.bytes().end());
  body.insert(body.end(), data.begin(), data.end());

  out->clear();
  out->insert(out->end(), std::begin(kPayloadMagic), std::end(kPayloadMagic));
  AppendU32(out, kPayloadVersion);
  AppendU32(out, static_cast<u32>(kind));
  AppendU64(out, cell_id_);
  AppendU64(out, bake_id_);
  AppendU8(out, static_cast<u8>(domain_));
  AppendU8(out, static_cast<u8>(tier_));
  AppendU16(out, 0);
  AppendU32(out, static_cast<u32>(archetype_records.size()));
  AppendU32(out, static_cast<u32>(column_records.size()));
  AppendU32(out, static_cast<u32>(prototype_names.size()));
  AppendU32(out, static_cast<u32>(instances_.size()));
  AppendU32(out, static_cast<u32>(strings.bytes().size()));
  AppendU64(out, data.size());
  AppendU64(out, Checksum(std::span<const u8>(body.data(), body.size())));
  out->insert(out->end(), body.begin(), body.end());
  return true;
}

std::string_view WorldCellPayload::String(u32 offset) const { return StringAt(strings, offset); }

std::span<const u8> WorldCellPayload::ColumnBytes(const WorldColumnRecord& column) const {
  if (column.data_offset > data.size() || data.size() - column.data_offset < column.data_bytes) {
    return {};
  }
  return std::span<const u8>(data.data() + column.data_offset,
                             static_cast<size_t>(column.data_bytes));
}

std::span<const u64> WorldCellPayload::StableIds(const WorldArchetypeRecord& archetype) const {
  if (archetype.stable_id_index > stable_ids.size() ||
      stable_ids.size() - archetype.stable_id_index < archetype.row_count) {
    return {};
  }
  return std::span<const u64>(stable_ids.data() + archetype.stable_id_index, archetype.row_count);
}

u32 WorldCellPayload::total_row_count() const {
  u32 total = static_cast<u32>(instances.size());
  for (const WorldArchetypeRecord& archetype : archetypes) total += archetype.row_count;
  return total;
}

bool DecodeCellPayload(std::span<const u8> bytes, WorldCellPayload* out, std::string* error) {
  if (!out) return false;
  Cursor header(bytes);
  const u8* magic = nullptr;
  if (!header.Take(sizeof(kPayloadMagic), &magic) ||
      std::memcmp(magic, kPayloadMagic, sizeof(kPayloadMagic)) != 0) {
    SetError(error, "cell payload: not an RXCELLPL file");
    return false;
  }
  const u32 version = header.U32();
  if (version != kPayloadVersion) {
    SetError(error, "cell payload: version " + std::to_string(version) + ", expected " +
                        std::to_string(kPayloadVersion));
    return false;
  }
  const u32 kind = header.U32();
  if (kind > static_cast<u32>(PayloadKind::kInstances)) {
    SetError(error, "cell payload: unknown kind " + std::to_string(kind));
    return false;
  }
  const u64 cell_id = header.U64();
  const u64 bake_id = header.U64();
  const u8 domain = header.U8();
  const u8 tier = header.U8();
  header.U16();
  const u32 archetype_count = header.U32();
  const u32 column_count = header.U32();
  const u32 prototype_count = header.U32();
  const u32 instance_count = header.U32();
  const u32 string_bytes = header.U32();
  const u64 data_bytes = header.U64();
  const u64 checksum = header.U64();
  if (!header.ok()) {
    SetError(error, "cell payload: truncated header");
    return false;
  }
  if (!ValidDomain(domain) || !ValidTier(tier) || tier == static_cast<u8>(Tier::kAbsent)) {
    SetError(error, "cell payload: unknown domain/tier");
    return false;
  }
  if (archetype_count > kMaximumArchetypes || column_count > kMaximumColumns ||
      prototype_count > kMaximumPrototypes || instance_count > kMaximumInstances ||
      string_bytes > kMaximumStringBytes || data_bytes > kMaximumDataBytes) {
    SetError(error, "cell payload: header counts out of range");
    return false;
  }
  if (kind == static_cast<u32>(PayloadKind::kEntities) &&
      (prototype_count != 0 || instance_count != 0)) {
    SetError(error, "cell payload: entity payload carries instance tables");
    return false;
  }
  if (kind == static_cast<u32>(PayloadKind::kInstances) &&
      (archetype_count != 0 || column_count != 0)) {
    SetError(error, "cell payload: instance payload carries entity tables");
    return false;
  }

  const size_t body_offset = header.offset();
  const u64 expected_body = static_cast<u64>(archetype_count) * kArchetypeBytes +
                            static_cast<u64>(column_count) * kColumnBytes +
                            static_cast<u64>(prototype_count) * kPrototypeBytes +
                            static_cast<u64>(instance_count) * kInstanceBytes + string_bytes +
                            data_bytes;
  if (bytes.size() - body_offset != expected_body) {
    SetError(error, "cell payload: body is " + std::to_string(bytes.size() - body_offset) +
                        " bytes, header describes " + std::to_string(expected_body));
    return false;
  }
  const std::span<const u8> body = bytes.subspan(body_offset);
  if (Checksum(body) != checksum) {
    SetError(error, "cell payload: checksum mismatch");
    return false;
  }

  *out = WorldCellPayload{};
  out->version = version;
  out->kind = static_cast<PayloadKind>(kind);
  out->cell_id = cell_id;
  out->bake_id = bake_id;
  out->domain = static_cast<Domain>(domain);
  out->tier = static_cast<Tier>(tier);

  Cursor cursor(body);
  out->archetypes.reserve(archetype_count);
  for (u32 i = 0; i < archetype_count; ++i) {
    WorldArchetypeRecord record;
    record.row_count = cursor.U32();
    record.column_first = cursor.U32();
    record.column_count = cursor.U32();
    record.stable_id_offset = cursor.U64();
    if (static_cast<u64>(record.column_first) + record.column_count > column_count) {
      SetError(error, "cell payload: archetype " + std::to_string(i) + " spans past the columns");
      return false;
    }
    const u64 stable_bytes = static_cast<u64>(record.row_count) * sizeof(u64);
    if (record.stable_id_offset > data_bytes ||
        data_bytes - record.stable_id_offset < stable_bytes) {
      SetError(error, "cell payload: archetype " + std::to_string(i) +
                          " stable ids fall outside the data section");
      return false;
    }
    out->archetypes.push_back(record);
  }

  out->columns.reserve(column_count);
  for (u32 i = 0; i < column_count; ++i) {
    WorldColumnRecord record;
    record.name = cursor.U32();
    record.stride = cursor.U32();
    record.layout_hash = cursor.U64();
    record.data_offset = cursor.U64();
    record.data_bytes = cursor.U64();
    if (record.name >= string_bytes) {
      SetError(error, "cell payload: column " + std::to_string(i) + " name is out of range");
      return false;
    }
    if (record.stride == 0) {
      SetError(error, "cell payload: column " + std::to_string(i) + " has a zero stride");
      return false;
    }
    if (record.data_offset > data_bytes || data_bytes - record.data_offset < record.data_bytes) {
      SetError(error, "cell payload: column " + std::to_string(i) +
                          " falls outside the data section");
      return false;
    }
    out->columns.push_back(record);
  }
  for (u32 a = 0; a < out->archetypes.size(); ++a) {
    const WorldArchetypeRecord& archetype = out->archetypes[a];
    for (u32 c = 0; c < archetype.column_count; ++c) {
      const WorldColumnRecord& column = out->columns[archetype.column_first + c];
      if (column.data_bytes != static_cast<u64>(column.stride) * archetype.row_count) {
        SetError(error, "cell payload: archetype " + std::to_string(a) + " column " +
                            std::to_string(c) + " byte count disagrees with its row count");
        return false;
      }
    }
  }

  out->prototypes.reserve(prototype_count);
  for (u32 i = 0; i < prototype_count; ++i) {
    WorldPrototypeRecord record;
    record.name = cursor.U32();
    if (record.name >= string_bytes) {
      SetError(error, "cell payload: prototype " + std::to_string(i) + " name is out of range");
      return false;
    }
    out->prototypes.push_back(record);
  }

  out->instances.reserve(instance_count);
  for (u32 i = 0; i < instance_count; ++i) {
    WorldInstanceRecord record;
    record.stable_id = cursor.U64();
    record.prototype = cursor.U32();
    record.position = cursor.ReadVec3();
    record.rotation.x = cursor.F32();
    record.rotation.y = cursor.F32();
    record.rotation.z = cursor.F32();
    record.rotation.w = cursor.F32();
    record.scale = cursor.F32();
    if (record.prototype >= prototype_count) {
      SetError(error, "cell payload: instance " + std::to_string(i) + " names unknown prototype " +
                          std::to_string(record.prototype));
      return false;
    }
    out->instances.push_back(record);
  }

  const u8* strings = nullptr;
  if (!cursor.Take(string_bytes, &strings)) {
    SetError(error, "cell payload: truncated string table");
    return false;
  }
  out->strings.insert(out->strings.end(), reinterpret_cast<const char*>(strings),
                      reinterpret_cast<const char*>(strings) + string_bytes);
  if (string_bytes != 0 && !StringTableTerminated(out->strings)) {
    SetError(error, "cell payload: string table is not terminated");
    return false;
  }

  const u8* data = nullptr;
  if (!cursor.Take(static_cast<size_t>(data_bytes), &data)) {
    SetError(error, "cell payload: truncated data section");
    return false;
  }
  out->data.insert(out->data.end(), data, data + data_bytes);
  if (!cursor.ok()) {
    SetError(error, "cell payload: truncated body");
    return false;
  }

  // Lift each archetype's stable ids out of the byte buffer, so reading one is
  // an array access rather than a cast through bytes that were never a u64.
  u32 total_ids = 0;
  for (const WorldArchetypeRecord& archetype : out->archetypes) total_ids += archetype.row_count;
  out->stable_ids.reserve(total_ids);
  for (WorldArchetypeRecord& archetype : out->archetypes) {
    archetype.stable_id_index = static_cast<u32>(out->stable_ids.size());
    Cursor ids(std::span<const u8>(out->data.data(), out->data.size()));
    const u8* skipped = nullptr;
    ids.Take(static_cast<size_t>(archetype.stable_id_offset), &skipped);
    for (u32 row = 0; row < archetype.row_count; ++row) out->stable_ids.push_back(ids.U64());
    if (!ids.ok()) {
      SetError(error, "cell payload: archetype stable ids run past the data section");
      return false;
    }
  }
  return true;
}

}  // namespace rx::world
