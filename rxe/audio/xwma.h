#ifndef RX_AUDIO_XWMA_H_
#define RX_AUDIO_XWMA_H_


#include "base/memory/unique_pointer.h"
#include "foundation/build_config/types.h"
#include "rxe/audio/audio_clip.h"

namespace rx::audio {

// The compressed audio containers the three games ship: Skyrim/Fallout 4 store
// music and ambience as xWMA (.xwm), voice lines as Bethesda's FUZ wrapper (a LIP
// lipsync block followed by an embedded xWMA), and Starfield uses Wwise media
// (.wem).
enum class CompressedKind { kXwma, kFuz, kWem };

// Opens a streaming decoder for a compressed container. PCM-tagged Wwise media is
// decoded natively; the WMA and Wwise codecs are routed to the optional FFmpeg
// backend (see ffmpeg_codec.h), which is null when the engine was built without
// it. Returns null when nothing can decode the data.
base::UniquePointer<Decoder> OpenCompressed(ByteSpan bytes, CompressedKind kind);

}  // namespace rx::audio

#endif  // RX_AUDIO_XWMA_H_
