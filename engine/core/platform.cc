#include "core/platform.h"

#include <stdlib.h>
#include <string.h>

#if defined(__linux__) && !defined(__ANDROID__)
#include <fcntl.h>
#include <unistd.h>
#endif

#include <base/option.h>

namespace rx {
namespace {

base::Option<bool> SteamDeckOpt{"platform.steamdeck", false, "RX_STEAMDECK"};

bool EnvIs(const char* name, const char* value) {
  const char* v = ::getenv(name);
  return v && ::strcmp(v, value) == 0;
}

#if defined(__linux__) && !defined(__ANDROID__)
bool ProbeSteamDeck() {
  // Not fs::ReadFile: sysfs reports every attribute as 4096 bytes and returns a
  // few, which that treats as a truncated read.
  char board[32] = {};
  const int fd = ::open("/sys/class/dmi/id/board_name", O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    const ssize_t got = ::read(fd, board, sizeof(board) - 1);
    ::close(fd);
    if (got > 0 && (::strncmp(board, "Jupiter", 7) == 0 || ::strncmp(board, "Galileo", 7) == 0))
      return true;
  }
  return EnvIs("SteamDeck", "1");
}
#endif

}  // namespace

bool IsSteamDeck() {
  if (SteamDeckOpt.overridden()) return SteamDeckOpt.get();
#if defined(__linux__) && !defined(__ANDROID__)
  static const bool deck = ProbeSteamDeck();
  return deck;
#else
  return false;
#endif
}

bool IsGamescope() {
  return EnvIs("XDG_CURRENT_DESKTOP", "gamescope") || ::getenv("GAMESCOPE_WAYLAND_DISPLAY");
}

}  // namespace rx
