#ifndef RX_FOUNDATION_SYSTEM_PLATFORM_H_
#define RX_FOUNDATION_SYSTEM_PLATFORM_H_

#include "foundation/build_config/export.h"

namespace rx {

// True on a Steam Deck (LCD "Jupiter" or OLED "Galileo"), in Game Mode and in
// Desktop Mode alike. The board is read from DMI, with Steam's own SteamDeck=1
// as the fallback for sandboxes that hide /sys. RX_STEAMDECK=0/1 overrides both,
// so the Deck defaults can be tried on a desktop and turned off on a Deck.
RX_FOUNDATION_EXPORT bool IsSteamDeck();

// True when the process runs under the gamescope compositor (Deck Game Mode, or
// gamescope started by hand on a desktop). Gamescope owns the output size and
// scales whatever it is given to fit, so a window there should cover it.
RX_FOUNDATION_EXPORT bool IsGamescope();

}  // namespace rx

#endif  // RX_FOUNDATION_SYSTEM_PLATFORM_H_
