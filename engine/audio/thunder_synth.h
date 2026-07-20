#ifndef RX_AUDIO_THUNDER_SYNTH_H_
#define RX_AUDIO_THUNDER_SYNTH_H_

#include <memory>

#include "audio/audio_clip.h"
#include "core/export.h"
#include "core/types.h"

namespace rx::audio {

// One-shot procedural thunder: a close strike opens with a band-passed crack
// transient, then hands over to a long brown-noise rumble whose envelope rolls
// through a few echo bumps before dying out. Distance shapes the character the
// way air does: the crack attenuates away first and the rumble's lowpass
// closes down, so a 3 km strike arrives as a deep muffled roll while a nearby
// one snaps. The caller owns the arrival delay (distance / ~343 m/s) and the
// spatial placement; this is just the sound.
//
// Returns a finite mono decoder at `output_rate` (no resampling in the mixer),
// or null for a zero rate, a rate above 768 kHz, or non-finite input;
// the mixer retires the voice at end of stream. `seed` varies the echo timing
// and roll character per strike, `energy` (0..1) scales length and weight.
RX_AUDIO_EXPORT std::unique_ptr<Decoder>
MakeThunder(u32 output_rate, u32 seed, f32 energy, f32 distance_m);

} // namespace rx::audio

#endif // RX_AUDIO_THUNDER_SYNTH_H_
