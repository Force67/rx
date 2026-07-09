#include "engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include <base/option.h>

#include "core/log.h"
#include "core/math.h"
#include "scene/components.h"

// Camera and input: routes per-frame input to the free-fly camera, the
// scripted camera drivers (orbit / replay / cinematic showcase / record), and
// the debug physics toss.
namespace rx {

// Camera / capture overrides, populated from the environment by
// base::InitOptionsFromEnv() at startup.
static base::Option<const char*> Cam{"cam", nullptr, "RX_CAM"};
static base::Option<bool> Orbit{"orbit", false, "RX_ORBIT"};
static base::Option<const char*> Record{"record", nullptr, "RX_RECORD"};
static base::Option<const char*> Replay{"replay", nullptr, "RX_REPLAY"};
static base::Option<bool> Showcase{"showcase", false, "RX_SHOWCASE"};
static base::Option<const char*> ShowcaseShots{"showcase.shots", nullptr, "RX_SHOWCASE_SHOTS"};
static base::Option<bool> ShowcaseQuit{"showcase.quit", false, "RX_SHOWCASE_QUIT"};

void Engine::UpdateCamera(f32 frame_delta) {
  if (!window_) return;
  const InputState& input = window_->input();

  bool kb = debug_ui_.wants_keyboard();
  bool allow_mouse = !debug_ui_.wants_mouse() || camera_.looking();
  bool allow_keyboard = !kb;
  camera_.Update(input, actions_, allow_mouse, allow_keyboard, frame_delta);
  window_->SetRelativeMouseMode(camera_.looking());

  DriveCamera(frame_delta);  // orbit / replay overrides + record

  if (actions_.pressed(Action::kToggleDebug) && !kb) debug_ui_.ToggleVisible();
  if (actions_.pressed(Action::kThrowDebug) && !kb) ThrowPhysicsCube();
}

void Engine::LookCameraAt(const Vec3& eye, const Vec3& center) {
  camera_.set_position(eye);
  Vec3 d = Normalize(center - eye);
  camera_.set_yaw_pitch(std::atan2(d.x, -d.z),
                        std::asin(std::clamp(d.y, -1.0f, 1.0f)));  // forward() convention
}

void Engine::DriveCamera(f32 dt) {
  if (!cam_init_) {
    cam_init_ = true;
    // RX_CAM="x,y,z,yaw,pitch" pins the camera for a framed capture (handy for
    // screenshots that must show a specific vantage).
    if (const char* c = Cam.get()) {
      Vec3 p{};
      f32 yaw = 0, pitch = 0;
      if (std::sscanf(c, "%f,%f,%f,%f,%f", &p.x, &p.y, &p.z, &yaw, &pitch) >= 3) {
        camera_.set_position(p);
        camera_.set_yaw_pitch(yaw, pitch);
      }
    }
    cam_orbit_ = bool(Orbit);
    if (const char* r = Record.get()) cam_record_ = std::fopen(r, "wb");
    if (const char* p = Replay.get()) {
      if (std::FILE* f = std::fopen(p, "rb")) {
        f32 key[7];
        while (std::fread(key, sizeof(f32), 7, f) == 7) {
          cam_replay_.push_back({key[0], {key[1], key[2], key[3]}, {key[4], key[5], key[6]}});
        }
        std::fclose(f);
        RX_INFO("camera replay: {} keys from {}", cam_replay_.size(), p);
      }
    }
    // RX_SHOWCASE flies a smooth cinematic pass over the scene in one take.
    // RX_SHOWCASE_SHOTS=<dir> writes a regression PNG at each marked beat;
    // RX_SHOWCASE_QUIT exits when the pass ends (headless benchmark).
    if (Showcase) {
      BuildShowcase();
      cam_showcase_ = !showcase_.empty();
      if (const char* d = ShowcaseShots.get()) showcase_shot_dir_ = d;
      showcase_quit_ = bool(ShowcaseQuit);
      if (cam_showcase_) {
        RX_INFO("showcase: {} waypoints, {:.1f}s{}", showcase_.size(), showcase_.duration(),
                showcase_shot_dir_.empty() ? "" : " (capturing)");
      }
    }
  }
  cam_time_ += dt;

  if (cam_showcase_) {
    f32 prev = cam_time_ - dt;
    ShowcasePose p = showcase_.Sample(cam_time_);
    LookCameraAt(p.eye, p.target);
    if (!showcase_shot_dir_.empty()) {
      std::string label;
      int idx = showcase_.CaptureCrossed(prev, cam_time_, &label);
      if (idx >= 0) {
        for (char& ch : label) {
          bool ok =
              (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
          if (!ok) ch = '_';
        }
        char path[1024];
        std::snprintf(path, sizeof(path), "%s/%02d_%s.png", showcase_shot_dir_.c_str(), idx,
                      label.c_str());
        renderer_.CaptureScreenshot(path);
        RX_INFO("showcase capture: {}", path);
      }
    }
    if (!showcase_done_) {
      // Benchmark steady-state render: skip the first second of warmup and any
      // half-second-plus frame (cold streaming/IO hitches, not GPU time). Real
      // mid-flight stutter (down to ~2 fps) is still counted, so it shows up.
      if (dt > 0 && dt < 0.5f && cam_time_ >= 1.0f) {
        showcase_dt_min_ = std::min(showcase_dt_min_, dt);
        showcase_dt_max_ = std::max(showcase_dt_max_, dt);
        showcase_bench_time_ += dt;
        ++showcase_frames_;
      }
      if (cam_time_ >= showcase_.duration()) {
        showcase_done_ = true;
        f32 avg =
            showcase_frames_ > 0 ? showcase_bench_time_ / static_cast<f32>(showcase_frames_) : 0.0f;
        RX_INFO("showcase done: {} frames over {:.1f}s, avg {:.0f} fps (min {:.0f}, max {:.0f})",
                showcase_frames_, showcase_bench_time_, avg > 0 ? 1.0f / avg : 0.0f,
                showcase_dt_max_ > 0 ? 1.0f / showcase_dt_max_ : 0.0f,
                showcase_dt_min_ > 0 ? 1.0f / showcase_dt_min_ : 0.0f);
        if (showcase_quit_) RequestQuit();
      }
    }
  } else if (cam_orbit_) {
    f32 a = cam_time_ * 0.4f;  // radians/sec
    Vec3 center{0.0f, 1.0f, 0.0f};
    LookCameraAt({center.x + std::cos(a) * 6.0f, 2.4f, center.z + std::sin(a) * 6.0f}, center);
  } else if (!cam_replay_.empty()) {
    // Linear interpolation between the bracketing keys for the current time.
    const CamKey* lo = &cam_replay_[0];
    const CamKey* hi = lo;
    for (const CamKey& k : cam_replay_) {
      if (k.t <= cam_time_) lo = &k;
      if (k.t >= cam_time_) {
        hi = &k;
        break;
      }
    }
    f32 span = hi->t - lo->t;
    f32 u = span > 1e-5f ? std::clamp((cam_time_ - lo->t) / span, 0.0f, 1.0f) : 0.0f;
    auto mix = [&](const Vec3& a, const Vec3& b) { return a + (b - a) * u; };
    LookCameraAt(mix(lo->pos, hi->pos), mix(lo->target, hi->target));
  }

  if (cam_record_) {
    Vec3 p = camera_.position(), t = camera_.target();
    f32 key[7] = {cam_time_, p.x, p.y, p.z, t.x, t.y, t.z};
    std::fwrite(key, sizeof(f32), 7, cam_record_);
    std::fflush(cam_record_);  // survive a timeout kill
  }
}

void Engine::BuildShowcase() {
  // A drone pass over the origin-anchored scene: establishing wide, descending
  // push-in, low skim, crane-up reveal. Every waypoint sits at ~zero velocity
  // (smoothstep), so each is a clean, well-framed still.
  auto wp = [](Vec3 eye, Vec3 look, f32 travel, bool capture, std::string label) {
    return ShowcaseCamera::Waypoint{eye, look, travel, capture, std::move(label)};
  };
  const Vec3 c{0, 0, 0};
  showcase_.Add(wp(c + Vec3{-18, 11, 8}, c + Vec3{-2, 2.5f, 0}, 0.0f, false, {}));
  showcase_.Add(wp(c + Vec3{-18, 11, 8}, c + Vec3{-2, 2.5f, 0}, 5.0f, true, "wide"));
  showcase_.Add(wp(c + Vec3{-7, 3.8f, -3}, c + Vec3{4, 1.5f, -6}, 5.0f, true, "descend"));
  showcase_.Add(wp(c + Vec3{-1, 2.2f, 2.5f}, c + Vec3{11, 1, -1}, 7.0f, true, "skim"));
  showcase_.Add(wp(c + Vec3{12, 9.2f, 7}, c + Vec3{3, 1.5f, 0}, 5.0f, true, "reveal"));
}

void Engine::ThrowPhysicsCube() {
  if (!physics_.initialized() || !physics_cube_mesh_) return;
  Vec3 forward = camera_.forward();
  Vec3 origin = camera_.position() + forward * 0.8f;
  // Wood-ish density: heavy enough to splash, light enough to float.
  physics::BodyId body =
      physics_.AddDynamicBox(origin, {0.25f, 0.25f, 0.25f}, 350.0f, forward * 14.0f);
  if (!body) return;
  ecs::Entity entity = world_.Create();
  world_.Add(entity, scene::Transform{.position = {origin.x, origin.y, origin.z}});
  world_.Add(entity, scene::Renderable{physics_cube_mesh_});
  physics_entities_.push_back({body, entity});
  if (window_ && input_map_.rumble) window_->SetRumble(0.35f, 0.7f, 180);  // toss kick
}

}  // namespace rx
