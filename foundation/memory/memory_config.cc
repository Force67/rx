#include "foundation/memory/memory_config.h"

#include <stdlib.h>

#include "base/strings/string_ref.h"
#include "base/strings/xstring.h"
#include "foundation/files/file_system.h"
#include "foundation/memory/chunk_pool.h"
#include "foundation/memory/frame_arena.h"
#include "foundation/memory/memory_tracker.h"

namespace rx {
namespace {

constexpr u64 kMiB = 1u << 20;

base::StringRef Trim(base::StringRef s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

bool ParseU64(base::StringRef s, u64& out) {
  if (s.empty()) return false;
  u64 value = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + static_cast<u64>(c - '0');
  }
  out = value;
  return true;
}

void SetBudget(MemoryConfig& config, base::StringRef name, u64 bytes) {
  for (auto& budget : config.budgets) {
    if (budget.name == name) {
      budget.bytes = bytes;
      return;
    }
  }
  config.budgets.push_back({base::String(name), bytes});
}

}  // namespace

void ParseMemoryConfigText(base::StringRef text, MemoryConfig& config) {
  base::String section;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t end = text.find('\n', start);
    base::StringRef line = text.substr(start, end == base::StringRef::npos ? end : end - start);
    start = end == base::StringRef::npos ? text.size() + 1 : end + 1;

    if (const size_t comment = line.find_first_of(";#"); comment != base::StringRef::npos) {
      line = line.substr(0, comment);
    }
    line = Trim(line);
    if (line.empty()) continue;

    if (line.front() == '[' && line.back() == ']') {
      section = base::String(Trim(line.substr(1, line.size() - 2)));
      continue;
    }

    const size_t equals = line.find('=');
    if (equals == base::StringRef::npos) continue;
    const base::StringRef key = Trim(line.substr(0, equals));
    u64 value = 0;
    if (!ParseU64(Trim(line.substr(equals + 1)), value)) continue;

    if (section == "arena" && key == "frame_mb") {
      config.frame_arena_bytes = static_cast<size_t>(value * kMiB);
    } else if (section == "pools" && key == "ecs_chunks") {
      config.ecs_chunk_reserve = static_cast<size_t>(value);
    } else if (section == "budgets") {
      SetBudget(config, key, value * kMiB);
    }
  }
}

void ApplyMemoryConfig(const MemoryConfig& config) {
  for (const auto& budget : config.budgets) {
    SetMemoryCategoryBudget(budget.name.c_str(), budget.bytes);
  }
  GlobalChunkPool().Reserve(config.ecs_chunk_reserve);
  MainFrameArena().Init(config.frame_arena_bytes);
}

}  // namespace rx
