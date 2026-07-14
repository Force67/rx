#ifndef RX_CORE_MEMORY_MEMORY_CONFIG_H_
#define RX_CORE_MEMORY_MEMORY_CONFIG_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/export.h"
#include "core/types.h"

namespace rx::mem {

// Declarative memory plan: initial reservations for the pools and soft
// per-category budgets for the tracker/HUD (the modern shape of the classic
// "pool config with expected sizes"). Defaults come from a platform preset;
// a memory.ini overlays them, so a shipped game can tune Steam Deck or mobile
// footprints without recompiling.
struct MemoryConfig {
  std::string preset = "desktop";
  size_t frame_arena_bytes = 8u << 20;
  size_t ecs_chunk_reserve = 256;  // 16 KiB chunks (4 MiB)
  struct Budget {
    std::string name;
    u64 bytes = 0;
  };
  std::vector<Budget> budgets;
};

// Preset table: "desktop" (default), "steamdeck", "mobile".
RX_CORE_EXPORT MemoryConfig DefaultMemoryConfig(std::string_view preset);

// Overlays ini text onto `config`. Format:
//   preset is chosen before parsing (RX_MEMORY_PRESET); sections:
//   [arena]   frame_mb = 8
//   [pools]   ecs_chunks = 256
//   [budgets] ecs = 64        ; MiB, one line per category
// Unknown keys are ignored so configs stay forward-compatible.
RX_CORE_EXPORT void ParseMemoryConfigText(std::string_view text, MemoryConfig& config);

// DefaultMemoryConfig(RX_MEMORY_PRESET or "desktop"), then overlays the file
// named by RX_MEMORY_INI (falling back to ./memory.ini when present).
RX_CORE_EXPORT MemoryConfig LoadMemoryConfig();

// Pushes the plan into the runtime: budgets into the tracker, chunk reserve
// into GlobalChunkPool, arena capacity into MainFrameArena.
RX_CORE_EXPORT void ApplyMemoryConfig(const MemoryConfig& config);

}  // namespace rx::mem

#endif  // RX_CORE_MEMORY_MEMORY_CONFIG_H_
