#ifndef RX_FOUNDATION_MEMORY_MEMORY_CONFIG_H_
#define RX_FOUNDATION_MEMORY_MEMORY_CONFIG_H_

#include <stddef.h>

#include "base/containers/vector.h"
#include "base/strings/string_ref.h"
#include "base/strings/xstring.h"
#include "foundation/build_config/export.h"
#include "foundation/build_config/types.h"

namespace rx {

// Declarative memory plan: initial reservations for the pools and soft
// per-category budgets for the tracker/HUD (the modern shape of the classic
// "pool config with expected sizes"). The defaults are a desktop's; the
// [memory.*] sections of the platform config files (docs/CONFIG.md) overlay
// them, so a shipped game tunes Steam Deck or mobile footprints without
// recompiling.
struct MemoryConfig {
  size_t frame_arena_bytes = 8u << 20;
  size_t ecs_chunk_reserve = 256;  // 16 KiB chunks (4 MiB)
  struct Budget {
    base::String name;
    u64 bytes = 0;
  };
  base::Vector<Budget> budgets = {{"assets", 2048ull << 20},
                                  {"render", 512ull << 20},
                                  {"ecs", 128ull << 20},
                                  {"audio", 64ull << 20}};
};

// Overlays ini text onto `config`. Format:
//   [arena]   frame_mb = 8
//   [pools]   ecs_chunks = 256
//   [budgets] ecs = 64        ; MiB, one line per category
// Unknown keys are ignored so configs stay forward-compatible.
RX_FOUNDATION_EXPORT void ParseMemoryConfigText(base::StringRef text, MemoryConfig& config);

// Pushes the plan into the runtime: budgets into the tracker, chunk reserve
// into GlobalChunkPool, arena capacity into MainFrameArena.
RX_FOUNDATION_EXPORT void ApplyMemoryConfig(const MemoryConfig& config);

}  // namespace rx

#endif  // RX_FOUNDATION_MEMORY_MEMORY_CONFIG_H_
