#include "core/memory/memory_config.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "core/memory/chunk_pool.h"
#include "core/memory/frame_arena.h"
#include "core/memory/memory_tracker.h"

namespace rx::mem {
namespace {

constexpr u64 kMiB = 1u << 20;

std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

bool ParseU64(std::string_view s, u64& out) {
  if (s.empty()) return false;
  u64 value = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + static_cast<u64>(c - '0');
  }
  out = value;
  return true;
}

void SetBudget(MemoryConfig& config, std::string_view name, u64 bytes) {
  for (auto& budget : config.budgets) {
    if (budget.name == name) {
      budget.bytes = bytes;
      return;
    }
  }
  config.budgets.push_back({std::string(name), bytes});
}

}  // namespace

MemoryConfig DefaultMemoryConfig(std::string_view preset) {
  MemoryConfig config;
  config.preset = preset;
  if (preset == "steamdeck") {
    config.frame_arena_bytes = 6u << 20;
    config.ecs_chunk_reserve = 192;
    config.budgets = {{"assets", 1024 * kMiB}, {"render", 384 * kMiB},
                      {"ecs", 96 * kMiB},      {"audio", 48 * kMiB}};
  } else if (preset == "mobile") {
    config.frame_arena_bytes = 4u << 20;
    config.ecs_chunk_reserve = 128;
    config.budgets = {{"assets", 512 * kMiB}, {"render", 256 * kMiB},
                      {"ecs", 64 * kMiB},     {"audio", 32 * kMiB}};
  } else {
    config.preset = "desktop";
    config.frame_arena_bytes = 8u << 20;
    config.ecs_chunk_reserve = 256;
    config.budgets = {{"assets", 2048 * kMiB}, {"render", 512 * kMiB},
                      {"ecs", 128 * kMiB},     {"audio", 64 * kMiB}};
  }
  return config;
}

void ParseMemoryConfigText(std::string_view text, MemoryConfig& config) {
  std::string section;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t end = text.find('\n', start);
    std::string_view line = text.substr(start, end == std::string_view::npos ? end : end - start);
    start = end == std::string_view::npos ? text.size() + 1 : end + 1;

    if (const size_t comment = line.find_first_of(";#"); comment != std::string_view::npos) {
      line = line.substr(0, comment);
    }
    line = Trim(line);
    if (line.empty()) continue;

    if (line.front() == '[' && line.back() == ']') {
      section = std::string(Trim(line.substr(1, line.size() - 2)));
      continue;
    }

    const size_t equals = line.find('=');
    if (equals == std::string_view::npos) continue;
    const std::string_view key = Trim(line.substr(0, equals));
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

MemoryConfig LoadMemoryConfig() {
  const char* preset = std::getenv("RX_MEMORY_PRESET");
  MemoryConfig config = DefaultMemoryConfig(preset ? preset : "desktop");

  const char* path = std::getenv("RX_MEMORY_INI");
  std::ifstream file(path ? path : "memory.ini");
  if (file) {
    std::ostringstream text;
    text << file.rdbuf();
    ParseMemoryConfigText(text.str(), config);
  }
  return config;
}

void ApplyMemoryConfig(const MemoryConfig& config) {
  for (const auto& budget : config.budgets) {
    SetCategoryBudget(budget.name.c_str(), budget.bytes);
  }
  GlobalChunkPool().Reserve(config.ecs_chunk_reserve);
  MainFrameArena().Init(config.frame_arena_bytes);
}

}  // namespace rx::mem
