#include "rxe/audio/audio_system.h"

#include <base/option.h>

#include "base/containers/vector.h"
#include "base/memory/move.h"
#include "base/memory/unique_pointer.h"
#include "base/optional.h"
#include "base/strings/string_ref.h"
#include "base/strings/xstring.h"
#include "foundation/logging/log.h"
#include "foundation/math/scalar.h"
#include "rxe/asset/vfs.h"

namespace rx::audio {
namespace {

// Suppression + level controls. Namespace scope so they register before
// InitOptionsFromEnv() runs (see base::Option). audio.mute fully suppresses the
// subsystem; audio.volume sets the master level.
base::Option<bool> Mute{"audio.mute", false, "RX_AUDIO_MUTE",
                        "open no audio device and silence all playback"};
base::Option<float> Volume{"audio.volume", 1.0f, "RX_AUDIO_VOLUME",
                           "master output volume, 0..1"};

// Pulls the extension (including the dot) off a path for decoder dispatch.
base::StringRef ExtensionOf(base::StringRef path) {
  const size_t dot = path.find_last_of('.');
  const size_t slash = path.find_last_of("/\\");
  if (dot == base::StringRef::npos || (slash != base::StringRef::npos && dot < slash))
    return {};
  return path.substr(dot);
}

}  // namespace

bool AudioSystem::Initialize(asset::Vfs* vfs) {
  vfs_ = vfs;
  muted_ = Mute.get();
  master_ = rx::Clamp(Volume.get(), 0.0f, 1.0f);

  if (muted_) {
    RX_INFO("audio: suppressed (RX_AUDIO_MUTE), running silent");
    return false;
  }
  const bool ok = device_.Open(&mixer_);
  mixer_.SetMasterGain(master_);
  return ok;
}

void AudioSystem::Shutdown() {
  mixer_.StopAll();
  device_.Close();
  clip_cache_.clear();
}

void AudioSystem::SetListener(const Vec3& position, const Vec3& forward, const Vec3& up) {
  // No device means nothing drains the mixer command queue, so skip enqueuing
  // (a headless server would otherwise grow the queue every frame).
  if (!active()) return;
  Listener listener;
  listener.position = position;
  listener.forward = forward;
  listener.up = up;
  mixer_.SetListener(listener);
}

bool AudioSystem::HasAsset(base::StringRef path) const {
  return vfs_ && vfs_->Contains(path);
}

bool AudioSystem::ReadAsset(base::StringRef path, base::Vector<u8>* out) {
  out->clear();
  if (!vfs_) return false;
  base::Optional<base::Vector<u8>> bytes = vfs_->Read(path);
  if (!bytes || bytes->size() == 0) return false;
  out->assign(bytes->data(), bytes->data() + bytes->size());
  return true;
}

const AudioClip* AudioSystem::GetClip(base::StringRef path) {
  const base::String key(path);
  if (const base::UniquePointer<AudioClip>* cached = clip_cache_.find(key)) {
    return (*cached)->valid() ? &**cached : nullptr;
  }

  base::Vector<u8> bytes;
  auto clip = base::MakeUnique<AudioClip>();
  if (ReadAsset(path, &bytes))
    *clip = DecodeClip(ByteSpan{bytes.data(), bytes.size()}, ExtensionOf(path));
  if (!clip->valid()) RX_WARN("audio: could not decode '{}'", key);
  const AudioClip* result = clip->valid() ? &*clip : nullptr;
  clip_cache_.insert(key, base::move(clip));
  return result;
}

base::UniquePointer<Decoder> AudioSystem::OpenStream(base::StringRef path) {
  base::Vector<u8> bytes;
  if (!ReadAsset(path, &bytes)) return nullptr;
  return OpenDecoder(ByteSpan{bytes.data(), bytes.size()}, ExtensionOf(path));
}

u32 AudioSystem::PlayUi(base::StringRef path, f32 gain) {
  if (!active()) return 0;
  const AudioClip* clip = GetClip(path);
  if (!clip) return 0;
  PlayParams params;
  params.gain = gain;
  params.positional = false;
  return mixer_.Play(MakeClipDecoder(*clip), params);
}

u32 AudioSystem::PlayAt(base::StringRef path, const Vec3& position, PlayParams params) {
  if (!active()) return 0;
  const AudioClip* clip = GetClip(path);
  if (!clip) return 0;
  params.positional = true;
  params.position = position;
  return mixer_.Play(MakeClipDecoder(*clip), params);
}

u32 AudioSystem::PlayLoop(base::StringRef path, PlayParams params) {
  if (!active()) return 0;
  base::UniquePointer<Decoder> decoder = OpenStream(path);
  if (!decoder) return 0;
  params.loop = true;
  return mixer_.Play(base::move(decoder), params);
}

void AudioSystem::Stop(u32 voice, f32 fade) {
  if (voice) mixer_.Stop(voice, fade);
}

void AudioSystem::StopAll() { mixer_.StopAll(); }

void AudioSystem::SetVoicePosition(u32 voice, const Vec3& position) {
  if (voice) mixer_.SetVoicePosition(voice, position);
}

void AudioSystem::SetVoiceGain(u32 voice, f32 gain) {
  if (voice) mixer_.SetVoiceGain(voice, gain);
}

void AudioSystem::SetMasterVolume(f32 volume) {
  master_ = rx::Clamp(volume, 0.0f, 1.0f);
  mixer_.SetMasterGain(master_);
}

}  // namespace rx::audio
