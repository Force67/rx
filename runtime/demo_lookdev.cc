#include "demo_lookdev.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <span>

#if defined(RX_HAS_IMGUI)
#include <imgui.h>
#endif

#include <base/option.h>

#include "asset/gltf_loader.h"
#include "asset/primitives.h"
#include "asset/scene_import.h"
#include "core/log.h"
#include "ecs/world.h"
#include "render/post/reference_compare.h"
#include "scene/components.h"

namespace rx {
namespace {

using render::HumanRegion;
using render::HumanSurfaceParameters;
using render::HumanTier;
using Compare = render::ReferenceCompare;

base::Option<const char*> LookdevSubject{"lookdev.subject", nullptr, "RX_LOOKDEV_SUBJECT"};
base::Option<const char*> LookdevShots{"lookdev.shots", nullptr, "RX_LOOKDEV_SHOTS"};
base::Option<bool> LookdevQuit{"lookdev.quit", false, "RX_LOOKDEV_QUIT"};
base::Option<const char*> LookdevReference{"lookdev.reference", nullptr, "RX_LOOKDEV_REFERENCE"};
base::Option<const char*> LookdevPreset{"lookdev.preset", nullptr, "RX_LOOKDEV_PRESET"};
// The neutral-parity contract, checkable end to end rather than only on the CPU
// mirror: RX_LOOKDEV_NEUTRAL=1 starts every part on the neutral parameter set
// and RX_LOOKDEV_HUMAN=0 takes the character model off entirely. Captured with
// the same rig, the two must be pixel-identical - if they are not, "turn the
// model on" silently re-shades everything that opted in.
base::Option<bool> LookdevNeutral{"lookdev.neutral", false, "RX_LOOKDEV_NEUTRAL"};
base::Option<bool> LookdevHuman{"lookdev.human", true, "RX_LOOKDEV_HUMAN"};
// The sweat / dual-normal A/B, scriptable so the demonstration is a capture and
// not a screenshot of someone dragging a slider. 0 = Ns == Nd exactly.
base::Option<double> LookdevSweat{"lookdev.sweat", 0.0, "RX_LOOKDEV_SWEAT"};

constexpr f32 kPi = 3.14159265358979323846f;
constexpr f32 kDeg = kPi / 180.0f;

// The OLAT set. One light at a time is not a stylistic choice: two lights at
// once make a parameter's effect unattributable, and every fit done that way
// lands on a compromise nobody chose. The last three stops exist to catch the
// failure the single-light stops cannot - a material that only holds together
// under one emitter shape, or under one light at a time.
constexpr LookdevDemo::LightStop kLightStops[] = {
    {"00 ambient only", {0, -1, 0}, LookdevDemo::LightStop::Kind::kNone, 0.0f, 0.0f},
    {"01 front soft panel", {0, -0.15f, 1}, LookdevDemo::LightStop::Kind::kRect, 0.45f, 2.8f},
    {"02 front hard point", {0, -0.15f, 1}, LookdevDemo::LightStop::Kind::kSphere, 0.01f, 4.0f},
    {"03 front sun", {0, -0.15f, 1}, LookdevDemo::LightStop::Kind::kSun, 0.0f, 4.0f},
    // The key stops sit on the side the camera stops orbit TOWARD, so a
    // three-quarter view sees the lit side and the terminator, not the back of
    // a shadow. Directions are the light's TRAVEL, engine sun convention.
    {"04 key 30", {-0.5f, -0.25f, 0.83f}, LookdevDemo::LightStop::Kind::kSun, 0.0f, 4.0f},
    {"05 key 45", {-0.7f, -0.25f, 0.66f}, LookdevDemo::LightStop::Kind::kSun, 0.0f, 4.0f},
    {"06 side 90", {-1, -0.1f, 0}, LookdevDemo::LightStop::Kind::kSun, 0.0f, 4.0f},
    {"07 grazing 110", {-0.94f, -0.05f, -0.34f}, LookdevDemo::LightStop::Kind::kSun, 0.0f, 4.0f},
    {"08 back", {0, -0.15f, -1}, LookdevDemo::LightStop::Kind::kSun, 0.0f, 4.0f},
    {"09 top", {0, -1, 0.15f}, LookdevDemo::LightStop::Kind::kSun, 0.0f, 4.0f},
    {"10 bottom", {0, 1, 0.15f}, LookdevDemo::LightStop::Kind::kSun, 0.0f, 4.0f},
    {"11 large rect side", {-1, -0.1f, 0}, LookdevDemo::LightStop::Kind::kRect, 0.6f, 4.0f},
    {"12 small point side", {-1, -0.1f, 0}, LookdevDemo::LightStop::Kind::kSphere, 0.008f, 4.0f},
    {"13 three lights", {-0.7f, -0.25f, 0.66f}, LookdevDemo::LightStop::Kind::kMulti, 0.2f, 4.0f},
};

// Frozen framings. "gameplay" and "lod" exist because a material optimized only
// for the close-up is the classic way to ship a face that falls apart in play.
constexpr LookdevDemo::CameraStop kCameraStops[] = {
    {"front", 0.0f, 0.0f, 0.85f, 32.0f},
    {"30", 30.0f, 0.0f, 0.85f, 32.0f},
    {"three-quarter", 45.0f, 0.0f, 0.85f, 32.0f},
    {"profile", 90.0f, 0.0f, 0.85f, 32.0f},
    {"close-up", 20.0f, -8.0f, 0.38f, 28.0f},
    {"gameplay", 25.0f, 0.0f, 2.5f, 45.0f},
    {"lod transition", 25.0f, 0.0f, 8.0f, 45.0f},
};

const char* RegionName(HumanRegion r) {
  switch (r) {
    case HumanRegion::kSkin: return "skin";
    case HumanRegion::kLips: return "lips";
    case HumanRegion::kTeeth: return "teeth";
    case HumanRegion::kGums: return "gums";
    case HumanRegion::kSclera: return "sclera";
    case HumanRegion::kCornea: return "cornea";
    case HumanRegion::kIris: return "iris";
    case HumanRegion::kTearline: return "tearline";
  }
  return "skin";
}

// Region from the source material name. A scanned head arrives as a handful of
// named submeshes and nothing else; guessing from the name is what makes the
// lab usable on a downloaded asset instead of only on hand-authored content.
// It is a guess - the panel lets it be overridden per material.
HumanRegion RegionFromName(std::string name) {
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  auto has = [&](const char* needle) { return name.find(needle) != std::string::npos; };
  if (has("cornea")) return HumanRegion::kCornea;
  if (has("iris") || has("pupil")) return HumanRegion::kIris;
  if (has("sclera") || has("eyeball") || has("eye")) return HumanRegion::kSclera;
  if (has("tear") || has("wetness") || has("moisture")) return HumanRegion::kTearline;
  if (has("teeth") || has("tooth") || has("dental")) return HumanRegion::kTeeth;
  if (has("gum") || has("gingiva")) return HumanRegion::kGums;
  if (has("lip") || has("mouth")) return HumanRegion::kLips;
  return HumanRegion::kSkin;
}

// The parameters the automated fitting is allowed to move, in the order the
// workflow says to fit them (frontal match, then specular shape, then
// retroreflection, then terminator, then grazing). Fitting them all at once is
// the documented way to converge on nothing.
struct FitField {
  const char* name;
  int stage;  // matches the fitting order in docs/CHARACTER_RENDERING.md
  f32 HumanSurfaceParameters::*member;
};

constexpr FitField kFitFields[] = {
    {"secondary_roughness_scale", 2, &HumanSurfaceParameters::secondary_roughness_scale},
    {"secondary_specular_weight", 2, &HumanSurfaceParameters::secondary_specular_weight},
    {"specular_fresnel_falloff", 2, &HumanSurfaceParameters::specular_fresnel_falloff},
    {"light_shape_response", 2, &HumanSurfaceParameters::light_shape_response},
    {"retroreflection_peak", 3, &HumanSurfaceParameters::retroreflection_peak},
    {"retroreflection_falloff", 3, &HumanSurfaceParameters::retroreflection_falloff},
    {"smooth_terminator_amount", 4, &HumanSurfaceParameters::smooth_terminator_amount},
    {"smooth_terminator_length", 4, &HumanSurfaceParameters::smooth_terminator_length},
    {"diffuse_fresnel_peak", 5, &HumanSurfaceParameters::diffuse_fresnel_peak},
    {"diffuse_fresnel_falloff", 5, &HumanSurfaceParameters::diffuse_fresnel_falloff},
    {"mean_free_path", 6, &HumanSurfaceParameters::mean_free_path},
    {"transmission", 6, &HumanSurfaceParameters::transmission},
};

}  // namespace

std::span<const LookdevDemo::LightStop> LookdevDemo::light_stops() { return kLightStops; }
std::span<const LookdevDemo::CameraStop> LookdevDemo::camera_stops() { return kCameraStops; }

// A procedural sweat / sebum normal map, bound as the SPECULAR normal (Ns).
// This is the one layer that proves the split is worth having: droplets have to
// bend the highlight without bending the diffuse, and a shared normal turns
// them into scarred geometry the moment the key light moves off axis. Generated
// rather than shipped so the demonstration has no asset dependency.
asset::Texture MakeSweatNormal(u32 size) {
  asset::Texture texture;
  texture.id = asset::MakeAssetId("builtin/lookdev/sweat_normal");
  texture.format = asset::TextureFormat::kRgba8;
  texture.width = size;
  texture.height = size;
  texture.is_srgb = false;  // a normal map is data, not colour
  texture.data.resize(static_cast<std::size_t>(size) * size * 4);

  // Deterministic droplet field: a fixed hash, so two runs of the lab produce
  // the same surface and a capture diff stays a renderer diff.
  auto hash2 = [](u32 x, u32 y) {
    u32 h = x * 374761393u + y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
  };
  const u32 cells = 24;
  const f32 cell = static_cast<f32>(size) / static_cast<f32>(cells);
  for (u32 py = 0; py < size; ++py) {
    for (u32 px = 0; px < size; ++px) {
      f32 nx = 0.0f, ny = 0.0f;
      const i32 cx = static_cast<i32>(static_cast<f32>(px) / cell);
      const i32 cy = static_cast<i32>(static_cast<f32>(py) / cell);
      for (i32 oy = -1; oy <= 1; ++oy) {
        for (i32 ox = -1; ox <= 1; ++ox) {
          const u32 gx = static_cast<u32>((cx + ox + cells) % cells);
          const u32 gy = static_cast<u32>((cy + oy + cells) % cells);
          const u32 h = hash2(gx, gy);
          if ((h & 3u) != 0u) continue;  // ~1 in 4 cells carries a droplet
          const f32 jx = (static_cast<f32>((h >> 8) & 255u) / 255.0f);
          const f32 jy = (static_cast<f32>((h >> 16) & 255u) / 255.0f);
          const f32 radius = cell * (0.18f + 0.22f * (static_cast<f32>((h >> 24) & 255u) / 255.0f));
          const f32 dx = static_cast<f32>(px) - (static_cast<f32>(gx) + jx) * cell;
          const f32 dy = static_cast<f32>(py) - (static_cast<f32>(gy) + jy) * cell;
          const f32 d = std::sqrt(dx * dx + dy * dy);
          if (d >= radius || radius <= 0.0f) continue;
          // Hemispherical bead: the slope grows toward the rim.
          const f32 t = d / radius;
          const f32 slope = t / std::sqrt(std::max(1.0f - t * t, 1e-3f));
          const f32 k = std::min(slope, 3.0f) / 3.0f;
          if (d > 1e-4f) {
            nx += (dx / d) * k;
            ny += (dy / d) * k;
          }
        }
      }
      Vec3 n{nx, ny, 1.0f};
      const f32 len = std::sqrt(n.x * n.x + n.y * n.y + 1.0f);
      const std::size_t o = (static_cast<std::size_t>(py) * size + px) * 4;
      texture.data[o + 0] = static_cast<u8>(std::clamp((n.x / len) * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f);
      texture.data[o + 1] = static_cast<u8>(std::clamp((n.y / len) * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f);
      texture.data[o + 2] = static_cast<u8>(std::clamp((1.0f / len) * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f);
      texture.data[o + 3] = 255;
    }
  }
  return texture;
}


// Reseeds a part's shaping controls while KEEPING the layer strengths. The
// presets describe a BRDF, not which maps a material happens to have bound: the
// bench binds a sweat normal on every skin part and holds it at zero, and a
// "reset to preset" that silently switched it on would make every A/B against
// the stock path a comparison of two different materials.
void ReseedShaping(HumanSurfaceParameters& params, HumanSurfaceParameters fresh) {
  fresh.specular_normal_strength = params.specular_normal_strength;
  fresh.region = params.region;
  params = fresh;
}

// -----------------------------------------------------------------------------

struct LookdevDemo::Impl {
  explicit Impl(EngineContext& c) : ctx(c) {}

  EngineContext& ctx;

  // --- subject ---------------------------------------------------------------
  // The authored materials are kept CPU-side so a slider can rewrite them and
  // push the change straight into the live uniform. This is also the thing that
  // gets saved and reloaded as a validation preset.
  struct Part {
    asset::Material material;
    HumanRegion region = HumanRegion::kSkin;
    HumanSurfaceParameters params;
    bool enabled = true;
    std::string name;
  };
  std::vector<Part> parts;
  std::vector<ecs::Entity> entities;
  Vec3 subject_center{0.0f, 1.6f, 0.0f};
  f32 subject_radius = 0.12f;   // metres; the head's bounding radius
  std::string subject_path;
  bool procedural_subject = false;
  // Ns for the sweat/sebum layer; bound on every skin part, off until dialled.
  asset::AssetId sweat_normal{};

  // --- rig -------------------------------------------------------------------
  int light_index = 5;      // key 45: the framing the frontal match starts from
  int camera_index = 0;
  HumanTier tier = HumanTier::kHero;
  bool freeze_exposure = true;
  f32 exposure_scale = 1.0f;
  bool ambient_fill = false;
  f32 gpu_budget_ms = 8.0f;  // OLAT purity: no environment unless asked for

  // --- comparison ------------------------------------------------------------
  std::string reference_path;
  std::string mask_path;
  char reference_input[256] = {};
  char mask_input[256] = {};

  // --- history ---------------------------------------------------------------
  // Parameter history: look-dev is a search, and a search without an undo is a
  // walk. Each entry is the whole part table, which is small and makes the
  // restore exact rather than field-by-field.
  std::vector<std::vector<Part>> history;
  std::size_t history_cursor = 0;
  f32 history_cooldown = 0.0f;
  bool dirty = false;
  bool pending_history = false;

  // --- automated fitting -----------------------------------------------------
  // Coordinate descent over kFitFields, measured against the reference error
  // summed over EVERY selected OLAT stop - fitting against one hero image is
  // the documented way to produce a material that only works in that image.
  struct Fit {
    bool running = false;
    int stage = 1;              // only fields at or below this stage move
    int field = 0;
    int probe = 0;              // 0 baseline, 1 minus, 2 plus
    int stop_cursor = 0;
    int settle = 0;             // frames to wait for the change to land
    f64 error[3] = {0, 0, 0};
    f32 step_scale = 0.25f;
    int passes_left = 0;
    HumanRegion region = HumanRegion::kSkin;
    std::vector<int> stops = {3, 5, 6, 7, 8};
    f32 saved = 0.0f;
    std::string log;
  } fit;

  // --- deterministic capture -------------------------------------------------
  struct Capture {
    bool running = false;
    bool finished = false;
    std::string dir;
    int light = 0;
    int camera = 0;
    int settle = 0;
  } capture;

  // --- helpers ---------------------------------------------------------------
  void LoadSubject();
  void BuildProceduralSubject();
  void ApplyPartsToRenderer();
  void PushHistory();
  void Undo();
  void Redo();
  void ApplyTier();
  void SelectLight(int index);
  void SelectCamera(int index);
  void StepFit();
  void StepCapture();
  void SavePreset(const std::string& path) const;
  bool LoadPreset(const std::string& path);
  void DrawPanel();
  render::CameraPose ResolveCamera() const;
  void EmitLights(render::FrameView& view);
};

// --- subject -----------------------------------------------------------------

void LookdevDemo::Impl::BuildProceduralSubject() {
  // The fallback subject. It is deliberately anatomical rather than a sphere:
  // the terminator, the transmission and the eye path all need curvature,
  // thin parts and an actual eyeball to say anything, and a lab that only
  // works once you have downloaded a 200 MB scan is a lab nobody opens.
  procedural_subject = true;
  subject_radius = 0.115f;
  auto add = [&](asset::Mesh mesh, const char* name, HumanRegion region, Vec3 offset,
                 const f32 color[3], f32 roughness) {
    Part part;
    part.name = name;
    part.region = region;
    part.params = render::HumanPreset(region);
    part.material.id = asset::MakeAssetId(std::string("builtin/lookdev/mat_") + name);
    part.material.name = name;
    std::memcpy(part.material.base_color_factor, color, sizeof(f32) * 3);
    part.material.base_color_factor[3] = 1.0f;
    part.material.roughness_factor = roughness;
    part.material.human = true;
    // The sweat layer is BOUND on every skin-family part but authored at
    // strength 0, so it is exactly neutral until someone drags the slider - and
    // dragging it never has to reallocate a binding set mid-session.
    part.material.human_params.specular_normal = sweat_normal;
    part.params.specular_normal_strength = 0.0f;
    render::HumanStore(part.params, part.material.human_params);
    // Skin-family regions also drive the screen-space diffusion; the analytic
    // BRDF handles the surface, the blur handles the transport.
    part.material.skin = region == HumanRegion::kSkin || region == HumanRegion::kLips ||
                         region == HumanRegion::kGums || region == HumanRegion::kSclera;
    // MakeSphere ships a submesh, MakeBox does not; give either one exactly one
    // submesh pointing at this part's material.
    if (mesh.lods[0].submeshes.empty()) {
      mesh.lods[0].submeshes.push_back(
          {0, static_cast<u32>(mesh.lods[0].indices.size()), part.material.id});
    } else {
      mesh.lods[0].submeshes[0].material = part.material.id;
    }

    if (!ctx.config->headless) {
      ctx.renderer->UploadMaterial(part.material);
      ctx.renderer->UploadMesh(mesh);
    }
    ecs::Entity e = ctx.world->Create();
    scene::Transform t;
    t.position[0] = subject_center.x + offset.x;
    t.position[1] = subject_center.y + offset.y;
    t.position[2] = subject_center.z + offset.z;
    ctx.world->Add(e, t);
    ctx.world->Add(e, scene::Renderable{mesh.id});
    entities.push_back(e);
    parts.push_back(std::move(part));
  };

  const f32 skin_color[3] = {0.62f, 0.44f, 0.35f};
  const f32 lip_color[3] = {0.55f, 0.25f, 0.24f};
  const f32 sclera_color[3] = {0.82f, 0.80f, 0.78f};
  const f32 iris_color[3] = {0.20f, 0.32f, 0.30f};
  const f32 teeth_color[3] = {0.86f, 0.84f, 0.79f};
  const f32 gum_color[3] = {0.60f, 0.28f, 0.28f};

  add(asset::MakeSphere(0.115f, 64, 96, asset::MakeAssetId("builtin/lookdev/cranium")), "skin",
      HumanRegion::kSkin, {0, 0, 0}, skin_color, 0.42f);
  // An ear-thickness proxy: a thin disc off the side is the only part of a head
  // that actually transmits, and without one the transmission lobe is untested.
  add(asset::MakeBox(0.006f, 0.035f, 0.022f, asset::MakeAssetId("builtin/lookdev/ear_l")), "ear_l",
      HumanRegion::kSkin, {-0.114f, 0.01f, 0.0f}, skin_color, 0.45f);
  add(asset::MakeBox(0.006f, 0.035f, 0.022f, asset::MakeAssetId("builtin/lookdev/ear_r")), "ear_r",
      HumanRegion::kSkin, {0.114f, 0.01f, 0.0f}, skin_color, 0.45f);
  add(asset::MakeBox(0.028f, 0.008f, 0.006f, asset::MakeAssetId("builtin/lookdev/lips")), "lips",
      HumanRegion::kLips, {0, -0.055f, -0.108f}, lip_color, 0.30f);
  add(asset::MakeBox(0.024f, 0.007f, 0.005f, asset::MakeAssetId("builtin/lookdev/teeth")), "teeth",
      HumanRegion::kTeeth, {0, -0.049f, -0.101f}, teeth_color, 0.18f);
  add(asset::MakeBox(0.024f, 0.004f, 0.004f, asset::MakeAssetId("builtin/lookdev/gums")), "gums",
      HumanRegion::kGums, {0, -0.043f, -0.100f}, gum_color, 0.35f);
  add(asset::MakeSphere(0.0125f, 32, 48, asset::MakeAssetId("builtin/lookdev/eye_l")), "eye_l",
      HumanRegion::kSclera, {-0.032f, 0.012f, -0.100f}, sclera_color, 0.10f);
  add(asset::MakeSphere(0.0125f, 32, 48, asset::MakeAssetId("builtin/lookdev/eye_r")), "eye_r",
      HumanRegion::kSclera, {0.032f, 0.012f, -0.100f}, sclera_color, 0.10f);
  add(asset::MakeSphere(0.0058f, 24, 32, asset::MakeAssetId("builtin/lookdev/iris_l")), "iris_l",
      HumanRegion::kIris, {-0.032f, 0.012f, -0.1085f}, iris_color, 0.22f);
  add(asset::MakeSphere(0.0058f, 24, 32, asset::MakeAssetId("builtin/lookdev/iris_r")), "iris_r",
      HumanRegion::kIris, {0.032f, 0.012f, -0.1085f}, iris_color, 0.22f);
}

void LookdevDemo::Impl::LoadSubject() {
  if (!ctx.config->headless) {
    const asset::Texture sweat = MakeSweatNormal(512);
    ctx.renderer->UploadTexture(sweat);
    sweat_normal = sweat.id;
  }
  std::string path;
  if (const char* explicit_path = LookdevSubject.get()) path = explicit_path;
  if (path.empty() && ctx.config && !ctx.config->scene_path.empty()) path = ctx.config->scene_path;
  if (path.empty()) {
    // What tools/get_head_scan.sh drops in, best fidelity first. The MPFB
    // avatar is deliberately NOT auto-picked: it carries morph targets, and the
    // viewer's morph-instance path currently hangs on it (reproduced on a clean
    // tree, so it predates this work). Pass it explicitly if you want it.
    const char* candidates[] = {"assets/head/head.glb", "assets/head/lps_head.glb"};
    for (const char* candidate : candidates) {
      if (std::filesystem::exists(candidate)) {
        path = candidate;
        break;
      }
    }
  }
  if (path.empty() || !std::filesystem::exists(path)) {
    RX_INFO("lookdev: no head asset found, using the procedural stand-in "
            "(run tools/get_head_scan.sh, or pass RX_LOOKDEV_SUBJECT=<file.glb>)");
    BuildProceduralSubject();
    return;
  }

  asset::ImportedScene scene;
  if (!asset::LoadGltfScene(path, &scene) || scene.meshes.empty()) {
    RX_WARN("lookdev: cannot load {}, using the procedural stand-in", path);
    BuildProceduralSubject();
    return;
  }
  subject_path = path;

  // Frame the subject on its own bounds. A scanned head arrives in whatever
  // scale and origin its author used, and a lab whose camera presets miss the
  // subject measures nothing.
  Vec3 lo{1e9f, 1e9f, 1e9f};
  Vec3 hi{-1e9f, -1e9f, -1e9f};
  for (const asset::ImportedScene::Instance& instance : scene.instances) {
    const asset::Mesh& mesh = scene.meshes[instance.mesh_index];
    for (const asset::Vertex& v : mesh.lods[0].vertices) {
      Vec3 p{v.position[0] * instance.scale + instance.position.x,
             v.position[1] * instance.scale + instance.position.y,
             v.position[2] * instance.scale + instance.position.z};
      lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
      hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
  }
  Vec3 extent{hi.x - lo.x, hi.y - lo.y, hi.z - lo.z};
  if (extent.y > 0.0f) {
    // Full-body scans exist and are usually posed, so the head is neither at
    // the model's x/z centre nor a fixed fraction up the box. Take the
    // CENTROID of the top slice instead: a camera preset that misses the
    // subject measures nothing, and "close-up" has to mean close-up on the
    // face of whatever was loaded.
    const bool full_body = extent.y > 0.9f;
    const f32 slice_floor = full_body ? hi.y - extent.y * 0.14f : lo.y;
    Vec3 sum{0, 0, 0};
    u32 count = 0;
    for (const asset::ImportedScene::Instance& instance : scene.instances) {
      const asset::Mesh& mesh = scene.meshes[instance.mesh_index];
      for (const asset::Vertex& v : mesh.lods[0].vertices) {
        Vec3 p{v.position[0] * instance.scale + instance.position.x,
               v.position[1] * instance.scale + instance.position.y,
               v.position[2] * instance.scale + instance.position.z};
        if (p.y < slice_floor) continue;
        sum = {sum.x + p.x, sum.y + p.y, sum.z + p.z};
        ++count;
      }
    }
    if (count > 0) {
      subject_center = {sum.x / static_cast<f32>(count), sum.y / static_cast<f32>(count),
                        sum.z / static_cast<f32>(count)};
      f32 r2 = 0.0f;
      for (const asset::ImportedScene::Instance& instance : scene.instances) {
        const asset::Mesh& mesh = scene.meshes[instance.mesh_index];
        for (const asset::Vertex& v : mesh.lods[0].vertices) {
          Vec3 p{v.position[0] * instance.scale + instance.position.x,
                 v.position[1] * instance.scale + instance.position.y,
                 v.position[2] * instance.scale + instance.position.z};
          if (p.y < slice_floor) continue;
          const Vec3 d{p.x - subject_center.x, p.y - subject_center.y, p.z - subject_center.z};
          r2 = std::max(r2, d.x * d.x + d.y * d.y + d.z * d.z);
        }
      }
      subject_radius = std::max(std::sqrt(r2), 1e-3f);
    } else {
      subject_center = {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
      subject_radius = 0.5f * std::max({extent.x, extent.y, extent.z});
    }
  }

  if (!ctx.config->headless) {
    for (const asset::Texture& texture : scene.textures) {
      if (texture.id) ctx.renderer->UploadTexture(texture);
    }
  }
  for (asset::Material& material : scene.materials) {
    Part part;
    part.name = material.name.empty() ? std::string("material") : material.name;
    part.region = RegionFromName(part.name);
    part.params = render::HumanPreset(part.region);
    part.material = material;
    part.material.human = true;
    part.material.human_params.specular_normal = sweat_normal;
    part.params.specular_normal_strength = 0.0f;
    render::HumanStore(part.params, part.material.human_params);
    part.material.skin = part.region == HumanRegion::kSkin || part.region == HumanRegion::kLips ||
                         part.region == HumanRegion::kGums || part.region == HumanRegion::kSclera;
    if (part.material.skin) {
      // Seed the diffusion profile from the same mean free path the analytic
      // lobe uses, so the two halves of the transport agree by construction.
      const f32 mfp_mm = part.params.mean_free_path * 1000.0f;
      part.material.skin_params.mfp[0] = mfp_mm;
      part.material.skin_params.mfp[1] = mfp_mm * 0.35f;
      part.material.skin_params.mfp[2] = mfp_mm * 0.2f;
    }
    if (!ctx.config->headless) ctx.renderer->UploadMaterial(part.material);
    parts.push_back(std::move(part));
  }
  if (!ctx.config->headless) {
    for (const asset::Mesh& mesh : scene.meshes) ctx.renderer->UploadMesh(mesh);
  }
  for (const asset::ImportedScene::Instance& instance : scene.instances) {
    const asset::Mesh& mesh = scene.meshes[instance.mesh_index];
    ecs::Entity e = ctx.world->Create();
    scene::Transform t;
    t.position[0] = instance.position.x;
    t.position[1] = instance.position.y;
    t.position[2] = instance.position.z;
    std::memcpy(t.rotation, instance.rotation, sizeof(f32) * 4);
    t.scale = instance.scale;
    ctx.world->Add(e, t);
    ctx.world->Add(e, scene::Renderable{mesh.id});
    entities.push_back(e);
  }
  RX_INFO("lookdev: {} ({} parts), head at ({:.2f}, {:.2f}, {:.2f}) r={:.3f}", path, parts.size(),
          subject_center.x, subject_center.y, subject_center.z, subject_radius);
}

void LookdevDemo::Impl::ApplyPartsToRenderer() {
  if (ctx.config->headless) return;
  for (Part& part : parts) {
    HumanSurfaceParameters resolved = part.params;
    resolved.region = part.region;
    render::HumanTierApply(tier, resolved);
    part.material.human = part.enabled;
    render::HumanStore(resolved, part.material.human_params);
    ctx.renderer->UpdateMaterial(part.material);
  }
}

// --- rig ---------------------------------------------------------------------

render::CameraPose LookdevDemo::Impl::ResolveCamera() const {
  const CameraStop& stop = kCameraStops[std::clamp(camera_index, 0,
                                                   static_cast<int>(std::size(kCameraStops)) - 1)];
  const f32 yaw = stop.yaw_degrees * kDeg;
  const f32 pitch = stop.pitch_degrees * kDeg;
  // Distances are authored for a head of ~0.115 m radius; scaling by the actual
  // subject keeps "close-up" meaning close-up on any asset.
  const f32 scale = subject_radius / 0.115f;
  const f32 distance = stop.distance * scale;
  render::CameraPose pose;
  pose.target = subject_center;
  pose.eye = {subject_center.x + std::sin(yaw) * std::cos(pitch) * distance,
              subject_center.y - std::sin(pitch) * distance,
              subject_center.z - std::cos(yaw) * std::cos(pitch) * distance};
  pose.fov_y = stop.fov_degrees * kDeg;
  return pose;
}

void LookdevDemo::Impl::SelectLight(int index) {
  const int count = static_cast<int>(std::size(kLightStops));
  light_index = ((index % count) + count) % count;
  const LightStop& stop = kLightStops[light_index];
  render::RenderSettings& s = ctx.renderer->settings();
  // OLAT means one light. Ambient and image-based lighting are a second,
  // omnidirectional light; leaving them on makes every stop a two-light stop.
  s.ambient = ambient_fill ? 0.06f : 0.0f;
  s.ibl = ambient_fill;
  s.sky = ambient_fill;
  const Vec3 t = Normalize(stop.travel);
  s.sun_direction = t;
  s.sun_color = {1.0f, 1.0f, 1.0f};
  const bool sun_driven = stop.kind == LightStop::Kind::kSun || stop.kind == LightStop::Kind::kMulti;
  s.sun_intensity = sun_driven ? stop.intensity : 0.0f;
  // A small hard source and a large soft one must be the same material; that
  // only holds if the sun's angular size is honest about which one it is.
  s.sun_angular_radius = stop.kind == LightStop::Kind::kSun ? 0.005f : 0.0f;
  ctx.scene_owns_sun = true;
}

void LookdevDemo::Impl::SelectCamera(int index) {
  const int count = static_cast<int>(std::size(kCameraStops));
  camera_index = ((index % count) + count) % count;
}

void LookdevDemo::Impl::EmitLights(render::FrameView& view) {
  const LightStop& stop = kLightStops[light_index];
  if (stop.kind == LightStop::Kind::kNone || stop.kind == LightStop::Kind::kSun) return;

  const Vec3 t = Normalize(stop.travel);
  // Place the emitter a fixed multiple of the subject size back along the light
  // direction, so a stop frames the same way on any asset.
  const f32 distance = std::max(subject_radius * 6.0f, 0.6f);
  // An area light's `intensity` is RADIANCE, so a 0.9 m panel and an 8 mm ball
  // at the same number differ by three orders of magnitude in how much light
  // they put on the face. The stops are authored as an ILLUMINANCE target (the
  // same number the sun stops use) and converted here through the emitter's own
  // solid angle - which is the only way "small hard source" versus "large soft
  // source" is a comparison of SHAPE rather than of brightness.
  auto emitter = [&](Vec3 travel, f32 size, f32 illuminance, u32 type) {
    render::PointLight light;
    const Vec3 d = Normalize(travel);
    const f32 area = (type == 3u) ? (2.0f * size) * (2.0f * size) : kPi * size * size;
    const f32 solid_angle = std::max(area / std::max(distance * distance, 1e-6f), 1e-6f);
    const f32 intensity = illuminance / solid_angle;
    light.pos_radius[0] = subject_center.x - d.x * distance;
    light.pos_radius[1] = subject_center.y - d.y * distance;
    light.pos_radius[2] = subject_center.z - d.z * distance;
    light.pos_radius[3] = distance * 4.0f;  // influence radius
    light.color_intensity[0] = 1.0f;
    light.color_intensity[1] = 1.0f;
    light.color_intensity[2] = 1.0f;
    light.color_intensity[3] = intensity;
    light.direction_type[0] = d.x;
    light.direction_type[1] = d.y;
    light.direction_type[2] = d.z;
    light.direction_type[3] = static_cast<f32>(type);
    light.params[0] = size;
    light.params[1] = size;
    view.lights.push_back(light);
  };

  if (stop.kind == LightStop::Kind::kSphere) {
    emitter(t, stop.size, stop.intensity, 2u);
  } else if (stop.kind == LightStop::Kind::kRect) {
    emitter(t, stop.size, stop.intensity, 3u);
  } else if (stop.kind == LightStop::Kind::kMulti) {
    // Key (the sun, set in SelectLight) plus a rect fill and a small rim: the
    // stop that catches a material that only reads under a single source.
    emitter({-0.8f, -0.1f, 0.6f}, stop.size, stop.intensity * 1.5f, 3u);   // fill, camera left
    emitter({-0.1f, -0.2f, -0.98f}, 0.02f, stop.intensity * 2.0f, 2u);     // rim, behind
  }
}

// --- history -----------------------------------------------------------------

void LookdevDemo::Impl::PushHistory() {
  history.resize(history_cursor);
  history.push_back(parts);
  if (history.size() > 64) history.erase(history.begin());
  history_cursor = history.size();
}

void LookdevDemo::Impl::Undo() {
  if (history_cursor <= 1) return;
  --history_cursor;
  parts = history[history_cursor - 1];
  ApplyPartsToRenderer();
}

void LookdevDemo::Impl::Redo() {
  if (history_cursor >= history.size()) return;
  parts = history[history_cursor];
  ++history_cursor;
  ApplyPartsToRenderer();
}

void LookdevDemo::Impl::ApplyTier() { ApplyPartsToRenderer(); }

// --- automated fitting -------------------------------------------------------
// Coordinate descent, one field at a time, measured against the reference error
// summed over every selected OLAT stop. Three properties make it a measurement
// rather than a preference:
//   * it only moves fields at or below the current STAGE, in the order the
//     workflow prescribes - a terminator fitted before the frontal match is
//     fitted against the wrong exposure;
//   * it scores over ALL selected lights at once, so a field cannot buy a win
//     under the key by losing under the rim;
//   * it accepts a step only if the summed error actually drops.
void LookdevDemo::Impl::StepFit() {
  if (!fit.running) return;
  auto& compare = ctx.renderer->reference_compare();
  if (!compare.has_reference()) {
    fit.log = "no reference loaded";
    fit.running = false;
    return;
  }
  const int field_count = static_cast<int>(std::size(kFitFields));
  // Skip fields above the current stage.
  while (fit.field < field_count && kFitFields[fit.field].stage > fit.stage) ++fit.field;
  if (fit.field >= field_count) {
    fit.field = 0;
    if (--fit.passes_left <= 0) {
      fit.running = false;
      fit.log += "  done";
      return;
    }
    fit.step_scale *= 0.5f;  // refine
    return;
  }

  Part* target = nullptr;
  for (Part& part : parts) {
    if (part.region == fit.region) {
      target = &part;
      break;
    }
  }
  if (!target) {
    fit.running = false;
    fit.log = "no part in that region";
    return;
  }

  const FitField& field = kFitFields[fit.field];
  const render::HumanRange range = render::HumanSafeRange(field.name);
  f32& value = target->params.*(field.member);

  // Settle: the parameter write lands in a uniform this frame, the metric it
  // produces is read the frame after. Two frames of settle keeps the score
  // attached to the value that produced it.
  if (fit.settle > 0) {
    --fit.settle;
    return;
  }

  // Accumulate this probe's error over the selected stops.
  if (fit.stop_cursor < static_cast<int>(fit.stops.size())) {
    const Compare::Stats stats = compare.stats(
        fit.region == HumanRegion::kSkin ? Compare::Region::kSkin : Compare::Region::kAll);
    fit.error[fit.probe] += stats.mean_squared_error;
    ++fit.stop_cursor;
    if (fit.stop_cursor < static_cast<int>(fit.stops.size())) {
      SelectLight(fit.stops[fit.stop_cursor]);
      fit.settle = 2;
      return;
    }
  }

  // Probe finished; move to the next probe or decide.
  const f32 span = range.hi - range.lo;
  const f32 step = span * fit.step_scale * 0.25f;
  if (fit.probe == 0) {
    fit.saved = value;
    value = std::clamp(fit.saved - step, range.lo, range.hi);
    fit.probe = 1;
  } else if (fit.probe == 1) {
    value = std::clamp(fit.saved + step, range.lo, range.hi);
    fit.probe = 2;
  } else {
    const bool minus = fit.error[1] < fit.error[0] && fit.error[1] <= fit.error[2];
    const bool plus = fit.error[2] < fit.error[0] && fit.error[2] < fit.error[1];
    if (minus) {
      value = std::clamp(fit.saved - step, range.lo, range.hi);
    } else if (plus) {
      value = std::clamp(fit.saved + step, range.lo, range.hi);
    } else {
      value = fit.saved;  // neither direction helped; leave it alone
    }
    char line[192];
    std::snprintf(line, sizeof(line), "%s %.4f (%s)\n", field.name, value,
                  minus ? "-" : (plus ? "+" : "="));
    fit.log += line;
    ++fit.field;
    fit.probe = 0;
    fit.error[0] = fit.error[1] = fit.error[2] = 0.0;
  }
  fit.stop_cursor = 0;
  fit.error[fit.probe] = 0.0;
  if (!fit.stops.empty()) SelectLight(fit.stops[0]);
  fit.settle = 2;
  ApplyPartsToRenderer();
}

// --- deterministic capture ---------------------------------------------------
// Walks the full validation matrix - every camera against every light - and
// writes one PNG per cell. Deterministic because the rig is frozen: same
// camera, same light, same material, same exposure, so a diff between two runs
// is a renderer change and nothing else.
void LookdevDemo::Impl::StepCapture() {
  if (!capture.running) return;
  if (capture.settle > 0) {
    --capture.settle;
    return;
  }
  const int lights = static_cast<int>(std::size(kLightStops));
  const int cameras = static_cast<int>(std::size(kCameraStops));
  if (capture.light >= lights) {
    capture.running = false;
    capture.finished = true;
    RX_INFO("lookdev: capture pass complete -> {}", capture.dir);
    return;
  }
  char name[512];
  std::snprintf(name, sizeof(name), "%s/lookdev_%02d_%s__%s.png", capture.dir.c_str(),
                capture.light, kLightStops[capture.light].name + 3,
                kCameraStops[capture.camera].name);
  // Spaces in a stop name would make the filename awkward to script over.
  for (char* c = name; *c; ++c)
    if (*c == ' ') *c = '_';
  ctx.renderer->CaptureScreenshot(name);
  if (++capture.camera >= cameras) {
    capture.camera = 0;
    ++capture.light;
    if (capture.light < lights) SelectLight(capture.light);
  }
  SelectCamera(capture.camera);
  // The capture has to outlast temporal accumulation: TAA and the upscaler both
  // need a few frames after a camera cut before the frame is the frame.
  capture.settle = 8;
}

// --- presets -----------------------------------------------------------------
// A flat key=value file, one section per part. Deliberately not a binary blob:
// a validation preset is something people diff, review and paste into a bug.

void LookdevDemo::Impl::SavePreset(const std::string& path) const {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    RX_WARN("lookdev: cannot write {}", path);
    return;
  }
  std::fprintf(f, "# rx character look-dev preset\n");
  std::fprintf(f, "model_version %u\n", render::kHumanModelVersion);
  std::fprintf(f, "subject %s\n", subject_path.empty() ? "<procedural>" : subject_path.c_str());
  std::fprintf(f, "light %d\ncamera %d\ntier %d\nexposure %.6f\n", light_index, camera_index,
               static_cast<int>(tier), exposure_scale);
  for (const Part& part : parts) {
    const HumanSurfaceParameters& p = part.params;
    std::fprintf(f, "\n[part] %s\nregion %s\nenabled %d\n", part.name.c_str(),
                 RegionName(part.region), part.enabled ? 1 : 0);
    std::fprintf(f, "diffuse_fresnel %.6f %.6f %.6f\n", p.diffuse_fresnel_peak,
                 p.diffuse_fresnel_falloff, p.diffuse_fresnel_tangent_falloff);
    std::fprintf(f, "retro %.6f %.6f %.6f\n", p.retroreflection_peak, p.retroreflection_falloff,
                 p.retroreflection_tangent_falloff);
    std::fprintf(f, "terminator %.6f %.6f\n", p.smooth_terminator_amount,
                 p.smooth_terminator_length);
    std::fprintf(f, "specular %.6f %.6f %.6f\n", p.specular_fresnel_falloff,
                 p.secondary_roughness_scale, p.secondary_specular_weight);
    std::fprintf(f, "transport %.6f %.6f %.6f %.6f %.6f\n", p.mean_free_path, p.subsurface_scale,
                 p.transmission, p.extinction_scale, p.thickness_scale);
    std::fprintf(f, "tint %.6f %.6f %.6f\n", p.transmission_tint[0], p.transmission_tint[1],
                 p.transmission_tint[2]);
    std::fprintf(f, "layer %.6f %.6f %.6f\n", p.corneal_wetness, p.cavity_occlusion,
                 p.specular_normal_strength);
    std::fprintf(f, "eye %.6f %.6f %.6f %.6f %.6f %.6f %.6f\n", p.iris_depth, p.iris_radius,
                 p.pupil_scale, p.limbal_ring_size, p.limbal_ring_power, p.cornea_ior,
                 p.iris_shadow_depth);
    std::fprintf(f, "residual %.6f\n", p.residual_weight);
  }
  std::fclose(f);
  RX_INFO("lookdev: preset written to {}", path);
}

bool LookdevDemo::Impl::LoadPreset(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  char line[512];
  Part* current = nullptr;
  while (std::fgets(line, sizeof(line), f)) {
    if (line[0] == '#' || line[0] == '\n') continue;
    char key[64] = {};
    if (std::sscanf(line, "%63s", key) != 1) continue;
    const char* rest = line + std::strlen(key);
    if (std::strcmp(key, "[part]") == 0) {
      char name[192] = {};
      std::sscanf(rest, " %191[^\n]", name);
      current = nullptr;
      for (Part& part : parts) {
        if (part.name == name) {
          current = &part;
          break;
        }
      }
      continue;
    }
    if (std::strcmp(key, "model_version") == 0) {
      u32 version = 0;
      std::sscanf(rest, " %u", &version);
      if (version != render::kHumanModelVersion) {
        // Fitted numbers are measurements against a specific model. Loading
        // them under a different one produces plausible-looking values that are
        // quietly wrong, which is harder to catch than a refusal.
        RX_WARN("lookdev: preset {} was fitted against model version {}, this build is {} - "
                "not loading",
                path, version, render::kHumanModelVersion);
        std::fclose(f);
        return false;
      }
      continue;
    }
    if (std::strcmp(key, "light") == 0) {
      std::sscanf(rest, " %d", &light_index);
      continue;
    }
    if (std::strcmp(key, "camera") == 0) {
      std::sscanf(rest, " %d", &camera_index);
      continue;
    }
    if (std::strcmp(key, "tier") == 0) {
      int t = 0;
      std::sscanf(rest, " %d", &t);
      tier = static_cast<HumanTier>(std::clamp(t, 0, 2));
      continue;
    }
    if (std::strcmp(key, "exposure") == 0) {
      std::sscanf(rest, " %f", &exposure_scale);
      continue;
    }
    if (!current) continue;
    HumanSurfaceParameters& p = current->params;
    if (std::strcmp(key, "diffuse_fresnel") == 0) {
      std::sscanf(rest, " %f %f %f", &p.diffuse_fresnel_peak, &p.diffuse_fresnel_falloff,
                  &p.diffuse_fresnel_tangent_falloff);
    } else if (std::strcmp(key, "retro") == 0) {
      std::sscanf(rest, " %f %f %f", &p.retroreflection_peak, &p.retroreflection_falloff,
                  &p.retroreflection_tangent_falloff);
    } else if (std::strcmp(key, "terminator") == 0) {
      std::sscanf(rest, " %f %f", &p.smooth_terminator_amount, &p.smooth_terminator_length);
    } else if (std::strcmp(key, "specular") == 0) {
      std::sscanf(rest, " %f %f %f", &p.specular_fresnel_falloff, &p.secondary_roughness_scale,
                  &p.secondary_specular_weight);
    } else if (std::strcmp(key, "transport") == 0) {
      std::sscanf(rest, " %f %f %f %f %f", &p.mean_free_path, &p.subsurface_scale, &p.transmission,
                  &p.extinction_scale, &p.thickness_scale);
    } else if (std::strcmp(key, "tint") == 0) {
      std::sscanf(rest, " %f %f %f", &p.transmission_tint[0], &p.transmission_tint[1],
                  &p.transmission_tint[2]);
    } else if (std::strcmp(key, "layer") == 0) {
      std::sscanf(rest, " %f %f %f", &p.corneal_wetness, &p.cavity_occlusion,
                  &p.specular_normal_strength);
    } else if (std::strcmp(key, "eye") == 0) {
      std::sscanf(rest, " %f %f %f %f %f %f %f", &p.iris_depth, &p.iris_radius, &p.pupil_scale,
                  &p.limbal_ring_size, &p.limbal_ring_power, &p.cornea_ior, &p.iris_shadow_depth);
    } else if (std::strcmp(key, "residual") == 0) {
      std::sscanf(rest, " %f", &p.residual_weight);
    } else if (std::strcmp(key, "enabled") == 0) {
      int e = 1;
      std::sscanf(rest, " %d", &e);
      current->enabled = e != 0;
    }
  }
  std::fclose(f);
  ApplyPartsToRenderer();
  SelectLight(light_index);
  RX_INFO("lookdev: preset loaded from {}", path);
  return true;
}

// --- panel -------------------------------------------------------------------

void LookdevDemo::Impl::DrawPanel() {
#if defined(RX_HAS_IMGUI)
  if (ImGui::GetCurrentContext() == nullptr) return;
  auto& compare = ctx.renderer->reference_compare();
  Compare::Settings& cs = compare.settings();

  ImGui::SetNextWindowSize(ImVec2(400, 900), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Lookdev - character reference lab")) {
    ImGui::TextWrapped("%s", subject_path.empty() ? "procedural stand-in (tools/get_head_scan.sh "
                                                    "fetches a real head)"
                                                  : subject_path.c_str());
    ImGui::Separator();

    // --- rig ---------------------------------------------------------------
    if (ImGui::CollapsingHeader("Rig", ImGuiTreeNodeFlags_DefaultOpen)) {
      int light = light_index;
      if (ImGui::SliderInt("OLAT light", &light, 0,
                           static_cast<int>(std::size(kLightStops)) - 1,
                           kLightStops[light_index].name)) {
        SelectLight(light);
      }
      ImGui::TextDisabled("left/right arrows cycle lights, up/down cycle cameras");
      int camera = camera_index;
      if (ImGui::SliderInt("Camera", &camera, 0,
                           static_cast<int>(std::size(kCameraStops)) - 1,
                           kCameraStops[camera_index].name)) {
        SelectCamera(camera);
      }
      if (ImGui::Checkbox("Ambient / IBL fill", &ambient_fill)) SelectLight(light_index);
      int tier_index = static_cast<int>(tier);
      if (ImGui::Combo("Quality tier", &tier_index, "Hero\0Standard\0Distant\0")) {
        tier = static_cast<HumanTier>(tier_index);
        ApplyTier();
      }
      // The quality preset caps the whole cast; the bench honours it so a
      // material fitted here is a material that can actually ship on the tier
      // it was fitted for.
      const int cap = static_cast<int>(ctx.renderer->settings().human_tier_cap);
      if (cap > tier_index) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                           "preset caps this hardware at tier %d", cap);
      }
      const f32 head_px = ImGui::GetIO().DisplaySize.y * 2.0f * subject_radius /
                          std::max(kCameraStops[camera_index].distance *
                                       (subject_radius / 0.115f) *
                                       std::tan(kCameraStops[camera_index].fov_degrees * kDeg * 0.5f) *
                                       2.0f,
                                   1e-4f);
      ImGui::Text("subject height: %.0f px -> tier %d suggested", head_px,
                  static_cast<int>(render::HumanTierForScreenHeight(head_px)));
    }

    // --- comparison --------------------------------------------------------
    if (ImGui::CollapsingHeader("Reference comparison", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::InputText("path", reference_input, sizeof(reference_input));
      ImGui::SameLine();
      if (ImGui::Button("Load")) {
        reference_path = reference_input;
        compare.LoadReference(*ctx.renderer->device(), reference_path);
      }
      ImGui::Text("reference: %s", compare.has_reference() ? "loaded" : "none");
      int mode = static_cast<int>(cs.mode);
      if (ImGui::Combo("mode", &mode,
                       "Off\0Side by side\0Wipe\0Linear difference\0Display difference\0Reference "
                       "only\0")) {
        cs.mode = static_cast<Compare::Mode>(mode);
      }
      ImGui::SliderFloat("split", &cs.split, 0.0f, 1.0f);
      ImGui::SliderFloat("difference gain", &cs.difference_gain, 1.0f, 64.0f);
      ImGui::SliderFloat("reference exposure", &cs.reference_exposure, 0.05f, 20.0f, "%.3f",
                         ImGuiSliderFlags_Logarithmic);
      ImGui::SliderFloat2("align scale", cs.uv_scale, 0.25f, 4.0f);
      ImGui::SliderFloat2("align offset", cs.uv_offset, -1.0f, 1.0f);
      int region = static_cast<int>(cs.region);
      if (ImGui::Combo("region mask", &region, "All\0Skin\0Eyes\0Lips\0Teeth\0")) {
        cs.region = static_cast<Compare::Region>(region);
      }
      ImGui::InputText("mask path", mask_input, sizeof(mask_input));
      ImGui::SameLine();
      if (ImGui::Button("Load mask")) {
        // r skin, g eyes, b lips, a teeth. Fitting one material against a mask
        // that mixes them converges on none of them.
        mask_path = mask_input;
        compare.LoadRegionMask(*ctx.renderer->device(), mask_path);
      }
      ImGui::Checkbox("collect error metric", &cs.collect_stats);
      cs.exposure_scale = exposure_scale;
      if (cs.collect_stats) {
        const Compare::Stats stats = compare.stats(cs.region);
        ImGui::Text("rmse %.5f   mae %.5f   coverage %.0f px", std::sqrt(stats.mean_squared_error),
                    stats.mean_absolute_error, stats.coverage);
      }
      ImGui::SliderFloat("frozen exposure", &exposure_scale, 0.05f, 8.0f, "%.3f",
                         ImGuiSliderFlags_Logarithmic);
    }

    // --- material ----------------------------------------------------------
    if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
      if (ImGui::Button("Undo")) Undo();
      ImGui::SameLine();
      if (ImGui::Button("Redo")) Redo();
      ImGui::SameLine();
      if (ImGui::Button("Reset to preset")) {
        for (Part& part : parts) ReseedShaping(part.params, render::HumanPreset(part.region));
        dirty = true;
      }
      ImGui::SameLine();
      if (ImGui::Button("Neutral (stock PBR)")) {
        for (Part& part : parts) ReseedShaping(part.params, render::HumanNeutral());
        dirty = true;
      }
      ImGui::TextDisabled("history %zu/%zu", history_cursor, history.size());

      for (std::size_t i = 0; i < parts.size(); ++i) {
        Part& part = parts[i];
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::TreeNodeEx(part.name.c_str(),
                              part.region == HumanRegion::kSkin ? ImGuiTreeNodeFlags_DefaultOpen
                                                                : 0)) {
          int region = static_cast<int>(part.region);
          if (ImGui::Combo("region", &region,
                           "Skin\0Lips\0Teeth\0Gums\0Sclera\0Cornea\0Iris\0Tearline\0")) {
            part.region = static_cast<HumanRegion>(region);
            ReseedShaping(part.params, render::HumanPreset(part.region));
            dirty = true;
          }
          if (ImGui::Checkbox("character model", &part.enabled)) dirty = true;

          HumanSurfaceParameters& p = part.params;
          auto slider = [&](const char* label, f32* value) {
            const render::HumanRange r = render::HumanSafeRange(label);
            if (ImGui::SliderFloat(label, value, r.lo, r.hi)) dirty = true;
          };
          ImGui::SeparatorText("1-2 frontal + specular shape");
          if (ImGui::SliderFloat("roughness", &part.material.roughness_factor, 0.02f, 1.0f))
            dirty = true;
          if (ImGui::ColorEdit3("base color", part.material.base_color_factor)) dirty = true;
          slider("specular_fresnel_falloff", &p.specular_fresnel_falloff);
          slider("secondary_roughness_scale", &p.secondary_roughness_scale);
          slider("secondary_specular_weight", &p.secondary_specular_weight);
          slider("light_shape_response", &p.light_shape_response);
          ImGui::SeparatorText("3 retroreflection");
          slider("retroreflection_peak", &p.retroreflection_peak);
          slider("retroreflection_falloff", &p.retroreflection_falloff);
          slider("retroreflection_tangent_falloff", &p.retroreflection_tangent_falloff);
          ImGui::SeparatorText("4 terminator");
          slider("smooth_terminator_amount", &p.smooth_terminator_amount);
          slider("smooth_terminator_length", &p.smooth_terminator_length);
          ImGui::SeparatorText("5 grazing");
          slider("diffuse_fresnel_peak", &p.diffuse_fresnel_peak);
          slider("diffuse_fresnel_falloff", &p.diffuse_fresnel_falloff);
          slider("diffuse_fresnel_tangent_falloff", &p.diffuse_fresnel_tangent_falloff);
          ImGui::SeparatorText("6 transport");
          slider("mean_free_path", &p.mean_free_path);
          slider("subsurface_scale", &p.subsurface_scale);
          slider("transmission", &p.transmission);
          if (ImGui::ColorEdit3("transmission tint", p.transmission_tint)) dirty = true;
          slider("extinction_scale", &p.extinction_scale);
          slider("thickness_scale", &p.thickness_scale);
          ImGui::SeparatorText("7 layers");
          slider("corneal_wetness", &p.corneal_wetness);
          slider("cavity_occlusion", &p.cavity_occlusion);
          slider("specular_normal_strength", &p.specular_normal_strength);
          if (part.region == HumanRegion::kCornea || part.region == HumanRegion::kIris ||
              part.region == HumanRegion::kSclera) {
            ImGui::SeparatorText("eye");
            slider("iris_depth", &p.iris_depth);
            slider("iris_radius", &p.iris_radius);
            slider("pupil_scale", &p.pupil_scale);
            slider("limbal_ring_size", &p.limbal_ring_size);
            slider("limbal_ring_power", &p.limbal_ring_power);
            slider("cornea_ior", &p.cornea_ior);
            slider("iris_shadow_depth", &p.iris_shadow_depth);
          }
          ImGui::SeparatorText("8 residual");
          slider("residual_weight", &p.residual_weight);
          ImGui::TreePop();
        }
        ImGui::PopID();
      }
    }

    // --- fitting -----------------------------------------------------------
    if (ImGui::CollapsingHeader("Automated fitting")) {
      ImGui::TextWrapped("Coordinate descent against the measured reference error, summed over "
                         "every selected OLAT stop. Fit stage by stage: a terminator fitted "
                         "before the frontal match is fitted against the wrong exposure.");
      ImGui::SliderInt("stage", &fit.stage, 1, 6);
      int region = static_cast<int>(fit.region);
      if (ImGui::Combo("region", &region, "Skin\0Lips\0Teeth\0Gums\0Sclera\0Cornea\0Iris\0Tearline\0"))
        fit.region = static_cast<HumanRegion>(region);
      ImGui::SliderFloat("step scale", &fit.step_scale, 0.02f, 1.0f);
      if (!fit.running && ImGui::Button("Fit (4 passes)")) {
        fit = Fit{};
        fit.region = static_cast<HumanRegion>(region);
        fit.running = true;
        fit.passes_left = 4;
        cs.collect_stats = true;
        PushHistory();
        if (!fit.stops.empty()) SelectLight(fit.stops[0]);
      }
      if (fit.running && ImGui::Button("Stop")) fit.running = false;
      if (!fit.log.empty()) ImGui::TextUnformatted(fit.log.c_str());
    }

    // --- captures / presets -------------------------------------------------
    if (ImGui::CollapsingHeader("Captures and presets")) {
      if (ImGui::Button("Capture the validation matrix")) {
        capture = Capture{};
        capture.running = true;
        capture.dir = LookdevShots.get() ? LookdevShots.get() : "build/lookdev-shots";
        std::error_code ec;
        std::filesystem::create_directories(capture.dir, ec);
        SelectLight(0);
        SelectCamera(0);
        capture.settle = 12;
      }
      if (capture.running) {
        ImGui::Text("capturing %s / %s", kLightStops[capture.light].name,
                    kCameraStops[capture.camera].name);
      }
      // Cost alarm. The hero tier is the one that can quietly grow past its
      // budget, because everything that makes it hero-tier is invisible in a
      // still and obvious in a frame time.
      const f32 frame_ms = ctx.renderer->gpu_frame_ms();
      ImGui::SliderFloat("gpu budget (ms)", &gpu_budget_ms, 1.0f, 33.0f);
      if (frame_ms > gpu_budget_ms) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "GPU %.2f ms OVER budget %.1f ms",
                           frame_ms, gpu_budget_ms);
      } else {
        ImGui::TextDisabled("gpu %.2f ms / %.1f ms", frame_ms, gpu_budget_ms);
      }
      if (ImGui::Button("Save preset")) SavePreset("lookdev_preset.ini");
      ImGui::SameLine();
      if (ImGui::Button("Load preset")) LoadPreset("lookdev_preset.ini");
    }
  }
  ImGui::End();
#endif
}

// --- public ------------------------------------------------------------------

LookdevDemo::LookdevDemo(EngineContext& ctx) : impl_(std::make_unique<Impl>(ctx)) {}
LookdevDemo::~LookdevDemo() = default;

void LookdevDemo::Create() {
  impl_->LoadSubject();
  if (bool(LookdevNeutral) || !bool(LookdevHuman)) {
    for (Impl::Part& part : impl_->parts) {
      ReseedShaping(part.params, render::HumanNeutral());
      part.enabled = bool(LookdevHuman);
    }
  }
  impl_->SelectLight(impl_->light_index);
  impl_->SelectCamera(impl_->camera_index);
  if (LookdevSweat.overridden()) {
    const f32 strength = static_cast<f32>(double(LookdevSweat));
    for (Impl::Part& part : impl_->parts) {
      if (part.material.skin) part.params.specular_normal_strength = strength;
    }
  }
  impl_->ApplyPartsToRenderer();
  impl_->PushHistory();

  render::RenderSettings& s = impl_->ctx.renderer->settings();
  // The bench is frozen on purpose: auto-exposure turns every material change
  // into an exposure change, and a comparison against a reference under a
  // moving exposure measures nothing.
  s.auto_exposure = false;
  s.sss = true;
  s.shadow_maps = true;
  // A reference bench renders native. An upscaler reconstructs detail, which is
  // exactly the thing under measurement; the tiers get exercised deliberately
  // through the LOD-distance camera stop instead.
  s.upscaler = render::UpscalerKind::kNone;
  s.render_scale = 1.0f;
  s.clouds = false;
  s.cloudscape = false;
  // The colour and exposure contract: everything between the shaded pixel and
  // the display that is not the tonemap comes off. Bloom, flare, aberration,
  // vignette, grain, DoF and motion blur are all differences between the
  // reference and the render that have nothing to do with the material, and a
  // comparison that includes them measures the lens.
  s.bloom = false;
  s.lens_flare = 0.0f;
  s.chromatic_aberration = 0.0f;
  s.vignette = 0.0f;
  s.film_grain = 0.0f;
  s.dof = false;
  s.motion_blur = false;

  auto& compare = impl_->ctx.renderer->reference_compare();
  if (const char* ref = LookdevReference.get()) {
    impl_->reference_path = ref;
    std::snprintf(impl_->reference_input, sizeof(impl_->reference_input), "%s", ref);
    compare.LoadReference(*impl_->ctx.renderer->device(), impl_->reference_path);
    compare.settings().mode = Compare::Mode::kWipe;
  }
  if (const char* preset = LookdevPreset.get()) impl_->LoadPreset(preset);
  if (const char* shots = LookdevShots.get()) {
    impl_->capture.running = true;
    impl_->capture.dir = shots;
    std::error_code ec;
    std::filesystem::create_directories(impl_->capture.dir, ec);
    impl_->SelectLight(0);
    impl_->SelectCamera(0);
    impl_->capture.settle = 16;
  }
}

void LookdevDemo::Update(f32 dt, const InputState& input, const ActionState& actions,
                         bool allow_keyboard, bool allow_mouse) {
  (void)actions;
  (void)allow_mouse;
  if (!allow_keyboard) return;
  if (input.key_pressed(Key::kArrowLeft)) impl_->SelectLight(impl_->light_index - 1);
  if (input.key_pressed(Key::kArrowRight)) impl_->SelectLight(impl_->light_index + 1);
  if (input.key_pressed(Key::kArrowUp)) impl_->SelectCamera(impl_->camera_index - 1);
  if (input.key_pressed(Key::kArrowDown)) impl_->SelectCamera(impl_->camera_index + 1);
  if (input.key_pressed(Key::kC)) {
    auto& cs = impl_->ctx.renderer->reference_compare().settings();
    cs.mode = static_cast<Compare::Mode>((static_cast<int>(cs.mode) + 1) % 6);
  }
  if (input.key_pressed(Key::kZ)) impl_->Undo();
  if (input.key_pressed(Key::kX)) impl_->Redo();
  if (input.key_pressed(Key::kR)) {
    for (Impl::Part& part : impl_->parts)
      ReseedShaping(part.params, render::HumanPreset(part.region));
    impl_->dirty = true;
  }
  // The history cooldown ticks in Emit, which runs whether or not input does.
  (void)dt;
}

void LookdevDemo::Emit(f32 dt, render::FrameView& view) {
  view.camera = impl_->ResolveCamera();
  impl_->EmitLights(view);
  impl_->DrawPanel();

  // A slider is dragged over many frames; coalescing the history entry keeps a
  // single drag as one undo step instead of sixty.
  if (impl_->dirty) {
    impl_->ApplyPartsToRenderer();
    impl_->dirty = false;
    impl_->history_cooldown = 0.35f;
    impl_->pending_history = true;
  }
  if (impl_->pending_history && impl_->history_cooldown <= 0.0f) {
    impl_->PushHistory();
    impl_->pending_history = false;
  }
  impl_->history_cooldown = std::max(impl_->history_cooldown - dt, 0.0f);

  impl_->StepFit();
  impl_->StepCapture();
}

bool LookdevDemo::capture_finished() const {
  return impl_->capture.finished && LookdevQuit;
}

}  // namespace rx
