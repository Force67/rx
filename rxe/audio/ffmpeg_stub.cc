#include "rxe/audio/ffmpeg_codec.h"

#include "base/memory/unique_pointer.h"
#include "foundation/logging/log.h"

// Compiled when the FFmpeg backend is off (the default, and the whole CI matrix):
// compressed audio that has no native decoder is skipped with a single, clear
// note rather than failing the build or crashing at runtime.
namespace rx::audio {

base::UniquePointer<Decoder> OpenFfmpegDecoder(ByteSpan) {
  static bool warned = false;
  if (!warned) {
    warned = true;
    RX_WARN(
        "audio: compressed formats (xWMA/FUZ/Wwise) need the FFmpeg backend; "
        "reconfigure with -DRX_AUDIO_FFMPEG=ON to hear them");
  }
  return nullptr;
}

bool FfmpegAvailable() { return false; }

}  // namespace rx::audio
