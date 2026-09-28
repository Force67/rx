#include "rxe/audio/mixer.h"

#include "base/algorithm.h"
#include "base/containers/vector.h"
#include "base/memory/move.h"
#include "base/memory/unique_pointer.h"
#include "base/threading/lock_guard.h"
#include "foundation/math/scalar.h"
#include "foundation/memory/memory_tracker.h"
#include <math.h>

namespace rx::audio {
namespace {

// Source frames decoded per top-up. Small enough that a one-shot does not over-
// decode, large enough that streaming voices rarely touch the decoder mid-block.
constexpr size_t kDecodeChunkFrames = 1024;
// Compact a voice's source buffer once this many frames have been consumed, so a
// long-running loop does not grow the buffer unbounded.
constexpr size_t kCompactThreshold = 8192;
const mem::Category kAudioCategory = mem::RegisterCategory("audio");

f32 RampStep(f32 seconds, u32 rate) {
  if (seconds <= 0.0f || rate == 0) return 1.0f;  // immediate
  return 1.0f / (seconds * static_cast<f32>(rate));
}

}  // namespace

u32 Mixer::Play(base::UniquePointer<Decoder> decoder, const PlayParams& params) {
  if (!decoder || decoder->channels() == 0 || decoder->sample_rate() == 0) return 0;
  base::LockGuard<base::Mutex> lock(mutex_);
  const u32 id = next_id_++;
  Command cmd;
  cmd.type = CmdType::kPlay;
  cmd.voice = id;
  cmd.decoder = base::move(decoder);
  cmd.params = params;
  pending_.push_back(base::move(cmd));
  return id;
}

void Mixer::Stop(u32 voice, f32 fade_out) {
  base::LockGuard<base::Mutex> lock(mutex_);
  Command cmd;
  cmd.type = CmdType::kStop;
  cmd.voice = voice;
  cmd.value = fade_out;
  pending_.push_back(base::move(cmd));
}

void Mixer::StopAll() {
  base::LockGuard<base::Mutex> lock(mutex_);
  Command cmd;
  cmd.type = CmdType::kStopAll;
  pending_.push_back(base::move(cmd));
}

void Mixer::SetVoiceGain(u32 voice, f32 gain) {
  base::LockGuard<base::Mutex> lock(mutex_);
  Command cmd;
  cmd.type = CmdType::kGain;
  cmd.voice = voice;
  cmd.value = gain;
  pending_.push_back(base::move(cmd));
}

void Mixer::SetVoicePosition(u32 voice, const Vec3& position) {
  base::LockGuard<base::Mutex> lock(mutex_);
  Command cmd;
  cmd.type = CmdType::kPosition;
  cmd.voice = voice;
  cmd.position = position;
  pending_.push_back(base::move(cmd));
}

void Mixer::SetListener(const Listener& listener) {
  base::LockGuard<base::Mutex> lock(mutex_);
  Command cmd;
  cmd.type = CmdType::kListener;
  cmd.listener = listener;
  pending_.push_back(base::move(cmd));
}

void Mixer::SetMasterGain(f32 gain) {
  base::LockGuard<base::Mutex> lock(mutex_);
  Command cmd;
  cmd.type = CmdType::kMaster;
  cmd.value = gain;
  pending_.push_back(base::move(cmd));
}

void Mixer::ApplyCommands() {
  base::Vector<Command>& commands = commands_scratch_;
  commands.clear();
  {
    base::LockGuard<base::Mutex> lock(mutex_);
    commands.swap(pending_);
  }
  for (Command& cmd : commands) {
    switch (cmd.type) {
      case CmdType::kPlay: {
        Voice voice;
        voice.id = cmd.voice;
        voice.params = cmd.params;
        voice.gain = cmd.params.gain;
        voice.src_channels = rx::Max(1u, cmd.decoder->channels());
        voice.decoder = base::move(cmd.decoder);
        if (cmd.params.fade_in > 0.0f) {
          voice.env = 0.0f;
          voice.env_rate = RampStep(cmd.params.fade_in, output_rate_);
        } else {
          voice.env = 1.0f;
          voice.env_rate = 0.0f;
        }
        voices_.push_back(base::move(voice));
        break;
      }
      case CmdType::kStop:
        for (Voice& v : voices_)
          if (v.id == cmd.voice) {
            v.stopping = true;
            v.env_rate = -RampStep(cmd.value, output_rate_);
          }
        break;
      case CmdType::kStopAll:
        for (Voice& v : voices_) {
          v.stopping = true;
          v.env_rate = -RampStep(0.05f, output_rate_);
        }
        break;
      case CmdType::kGain:
        for (Voice& v : voices_)
          if (v.id == cmd.voice) v.gain = cmd.value;
        break;
      case CmdType::kPosition:
        for (Voice& v : voices_)
          if (v.id == cmd.voice) v.params.position = cmd.position;
        break;
      case CmdType::kListener:
        listener_ = cmd.listener;
        break;
      case CmdType::kMaster:
        master_ = cmd.value;
        break;
    }
  }
}

bool Mixer::FillSource(Voice& voice, size_t frames) {
  const u32 ch = voice.src_channels;
  // Reclaim space ahead of the read cursor before topping up.
  if (voice.head > kCompactThreshold) {
    voice.src.erase(voice.src.begin(),
                    voice.src.begin() + static_cast<ptrdiff_t>(voice.head) * ch);
    voice.head = 0;
  }
  base::Vector<float>& chunk = decode_scratch_;
  chunk.resize(static_cast<size_t>(kDecodeChunkFrames) * ch);
  while (voice.src.size() / ch - voice.head < frames) {
    if (voice.ended) return voice.src.size() / ch > voice.head;
    u32 got = voice.decoder->Read(chunk.data(), kDecodeChunkFrames);
    if (got == 0) {
      if (voice.params.loop && voice.decoder->Rewind()) continue;
      voice.ended = true;
      return voice.src.size() / ch > voice.head;
    }
    voice.src.insert(voice.src.end(), chunk.begin(),
                     chunk.begin() + static_cast<ptrdiff_t>(got) * ch);
  }
  return true;
}

void Mixer::MixInto(float* out, u32 frames) {
  mem::CategoryScope mem_scope(kAudioCategory);
  ApplyCommands();
  base::Fill(out, out + static_cast<size_t>(frames) * 2, 0.0f);

  for (Voice& voice : voices_) {
    const u32 ch = voice.src_channels;
    const f64 ratio = static_cast<f64>(voice.decoder->sample_rate()) / output_rate_;

    // One pan/gain solve per block, then ramped per frame for a click-free move.
    StereoGains target;
    if (voice.params.positional) {
      target = PanForSource(listener_, voice.params.position, voice.params.atten);
      target.left *= voice.gain;
      target.right *= voice.gain;
    } else {
      target = {voice.gain, voice.gain};
    }
    const f32 l_step = (target.left - voice.current.left) / static_cast<f32>(frames);
    const f32 r_step = (target.right - voice.current.right) / static_cast<f32>(frames);

    for (u32 i = 0; i < frames; ++i) {
      // Need head and head+1 for linear interpolation.
      if (!FillSource(voice, 2)) break;
      const size_t available = voice.src.size() / ch;
      if (available <= voice.head) break;
      const size_t i0 = voice.head;
      const size_t i1 = rx::Min(i0 + 1, available - 1);
      const f32 t = static_cast<f32>(voice.frac);

      f32 sl, sr;
      if (ch == 1) {
        const f32 s =
            voice.src[i0] * (1.0f - t) + voice.src[i1] * t;
        sl = sr = s;
      } else {
        sl = voice.src[i0 * ch + 0] * (1.0f - t) + voice.src[i1 * ch + 0] * t;
        sr = voice.src[i0 * ch + 1] * (1.0f - t) + voice.src[i1 * ch + 1] * t;
      }

      // Envelope (fade in/out). A stopping voice that reaches zero is done.
      voice.env += voice.env_rate;
      if (voice.env_rate > 0.0f && voice.env >= 1.0f) {
        voice.env = 1.0f;
        voice.env_rate = 0.0f;
      }
      if (voice.env <= 0.0f && voice.stopping) {
        voice.env = 0.0f;
        voice.dead = true;
        break;
      }
      const f32 e = rx::Clamp(voice.env, 0.0f, 1.0f);

      if (voice.params.positional) {
        const f32 mono = 0.5f * (sl + sr);
        out[i * 2 + 0] += mono * voice.current.left * e;
        out[i * 2 + 1] += mono * voice.current.right * e;
      } else {
        out[i * 2 + 0] += sl * voice.current.left * e;
        out[i * 2 + 1] += sr * voice.current.right * e;
      }
      voice.current.left += l_step;
      voice.current.right += r_step;

      // Advance the fractional read cursor by the resample ratio.
      voice.frac += ratio;
      while (voice.frac >= 1.0) {
        voice.frac -= 1.0;
        ++voice.head;
      }
    }
  }

  // Drop finished voices (end of stream, or a fade-out that completed).
  base::EraseIf(voices_, [](const Voice& v) {
    const size_t available = v.src.size() / rx::Max(1u, v.src_channels);
    return v.dead || (v.ended && available <= v.head);
  });

  // Master gain and a soft clip, so a dense mix saturates gracefully instead of
  // wrapping. tanh is gentle near unity and only bends the peaks.
  const f32 m = master_;
  for (u32 i = 0; i < frames * 2; ++i) {
    f32 s = out[i] * m;
    if (s > 1.0f || s < -1.0f) s = ::tanhf(s);
    out[i] = s;
  }
}

}  // namespace rx::audio
