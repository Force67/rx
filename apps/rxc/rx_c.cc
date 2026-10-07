// rxc: the C ABI over rx::app::Host. See rx_c.h for the contract.

#include "apps/rxc/rx_c.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <string.h>

#include "base/containers/unordered_map.h"
#include "base/containers/vector.h"
#include "base/memory/move.h"
#include "base/memory/unique_pointer.h"
#include "foundation/logging/log.h"
#include "foundation/math/math.h"
#include "foundation/math/scalar.h"
#include "foundation/strings/format.h"
#include "rxe/app/application.h"
#include "rxe/app/host.h"
#include "rxe/app/services.h"
#include "rxe/asset/asset_id.h"
#include "rxe/asset/material.h"
#include "rxe/asset/mesh.h"
#include "rxe/asset/texture.h"
#include "rxe/asset/vfs.h"
#include "rxe/audio/audio_clip.h"
#include "rxe/audio/audio_system.h"
#include "rxe/audio/mixer.h"
#include "rxe/render/core/renderer.h"
#include "rxe/render/geometry/particles.h"
#include "rxe/ui/imgui/imgui_renderer.h"
#include "rxe/ui/imgui/imgui_theme.h"
#include "rxe/ui/window/window.h"

namespace rx::rxc {
namespace {

// A decoder over a clip the shim keeps alive for the whole run, so firing the
// same sound twice copies no samples.
class SharedClipDecoder final : public audio::Decoder {
 public:
  explicit SharedClipDecoder(const audio::AudioClip* clip) : clip_(clip) {}
  u32 channels() const override { return clip_->channels; }
  u32 sample_rate() const override { return clip_->sample_rate; }
  u64 frame_count() const override { return clip_->frames(); }
  u32 Read(float* out, u32 frames) override {
    const u64 total = clip_->frames();
    const u64 left = total - cursor_;
    const u32 n = static_cast<u32>(left < frames ? left : frames);
    memcpy(out, clip_->samples.data() + cursor_ * clip_->channels,
           sizeof(float) * n * clip_->channels);
    cursor_ += n;
    return n;
  }
  bool Rewind() override {
    cursor_ = 0;
    return true;
  }

 private:
  const audio::AudioClip* clip_;
  u64 cursor_ = 0;
};

struct MeshRecord {
  asset::Mesh mesh;  // kept for dynamic updates: same topology, new vertices
  u64 hash = 0;
};

class CApp;

struct State {
  CApp* app = nullptr;
  app::Host* host = nullptr;
  app::Services* services = nullptr;
  rxc_callbacks callbacks{};
  double time = 0.0;
  bool in_frame = false;
  bool imgui = false;      // a context exists (windowed or offscreen)
  bool imgui_sdl = false;  // the SDL3 platform backend drives it
  bool imgui_open = false; // between NewFrame and Render
  ui::ImGuiRenderer imgui_renderer;
  render::FrameView* view = nullptr;
  base::Vector<asset::AssetId> textures;
  base::Vector<asset::Material> materials;
  base::Vector<base::UniquePointer<MeshRecord>> meshes;
  base::Vector<base::UniquePointer<audio::AudioClip>> sounds;
  base::Vector<render::DebugLine> lines;
  base::UnorderedMap<u64, Mat4> prev_transforms;
  base::UnorderedMap<u64, Mat4> next_transforms;
  ImFont* font = nullptr;
  float wheel = 0.0f;
  bool rendering = false;  // the host draws frames: windowed or offscreen
};

State* g = nullptr;

// Monotonic ids for content created through the shim, salted so they never
// collide with engine assets.
asset::AssetId NewId(const char* kind) {
  static u32 counter = 0;
  return asset::MakeAssetId(StrFormat("rxc/{}/{}", kind, ++counter));
}

Vec3 V3(const float v[3]) { return Vec3{v[0], v[1], v[2]}; }

void FillMaterial(asset::Material& m, const rxc_material_desc& d) {
  for (int i = 0; i < 4; ++i) m.base_color_factor[i] = d.base_color[i];
  m.roughness_factor = d.roughness;
  m.metallic_factor = d.metallic;
  for (int i = 0; i < 3; ++i) m.emissive_factor[i] = d.emissive[i];
  m.base_color = asset::AssetId{};
  m.emissive = asset::AssetId{};
  if (d.base_color_texture && d.base_color_texture <= g->textures.size())
    m.base_color = g->textures[d.base_color_texture - 1];
  if (d.emissive_texture && d.emissive_texture <= g->textures.size())
    m.emissive = g->textures[d.emissive_texture - 1];
  m.clearcoat = d.clearcoat;
  m.clearcoat_roughness = 0.05f;
  m.specular_strength = d.specular > 0.0f ? d.specular : 1.0f;
  m.two_sided = d.two_sided != 0;
  m.alpha_mode = d.blend ? asset::AlphaMode::kBlend : asset::AlphaMode::kOpaque;
}

// Per-triangle tangents from the uv gradient, accumulated per vertex.
void FillVertices(asset::MeshLod& lod, const rxc_vertex* src, u32 n, const u32* idx, u32 ni) {
  lod.vertices.resize(n);
  for (u32 i = 0; i < n; ++i) {
    asset::Vertex& v = lod.vertices[i];
    memcpy(v.position, src[i].position, sizeof(v.position));
    memcpy(v.normal, src[i].normal, sizeof(v.normal));
    memcpy(v.uv, src[i].uv, sizeof(v.uv));
    v.color = src[i].color;
    v.tangent[0] = v.tangent[1] = v.tangent[2] = 0.0f;
    v.tangent[3] = 1.0f;
  }
  for (u32 t = 0; t + 2 < ni; t += 3) {
    asset::Vertex* tri[3] = {&lod.vertices[idx[t]], &lod.vertices[idx[t + 1]],
                             &lod.vertices[idx[t + 2]]};
    const Vec3 p0{tri[0]->position[0], tri[0]->position[1], tri[0]->position[2]};
    const Vec3 e1 = Vec3{tri[1]->position[0], tri[1]->position[1], tri[1]->position[2]} - p0;
    const Vec3 e2 = Vec3{tri[2]->position[0], tri[2]->position[1], tri[2]->position[2]} - p0;
    const float du1 = tri[1]->uv[0] - tri[0]->uv[0], dv1 = tri[1]->uv[1] - tri[0]->uv[1];
    const float du2 = tri[2]->uv[0] - tri[0]->uv[0], dv2 = tri[2]->uv[1] - tri[0]->uv[1];
    const float det = du1 * dv2 - du2 * dv1;
    if (det == 0.0f) continue;
    const float r = 1.0f / det;
    const Vec3 tan = (e1 * dv2 - e2 * dv1) * r;
    for (asset::Vertex* v : tri) {
      v->tangent[0] += tan.x;
      v->tangent[1] += tan.y;
      v->tangent[2] += tan.z;
    }
  }
  for (asset::Vertex& v : lod.vertices) {
    const Vec3 nrm{v.normal[0], v.normal[1], v.normal[2]};
    Vec3 tan{v.tangent[0], v.tangent[1], v.tangent[2]};
    tan = tan - nrm * Dot(nrm, tan);  // Gram-Schmidt onto the surface
    float len = Length(tan);
    if (len < 1e-6f) {
      // No usable uv gradient: any vector perpendicular to the normal.
      tan = Cross((nrm.y < 0.0f ? -nrm.y : nrm.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0}, nrm);
      len = Length(tan);
    }
    tan = tan * (1.0f / len);
    v.tangent[0] = tan.x;
    v.tangent[1] = tan.y;
    v.tangent[2] = tan.z;
    v.tangent[3] = 1.0f;
  }
}

void ComputeBounds(asset::Mesh& mesh) {
  const auto& verts = mesh.lods[0].vertices;
  if (verts.empty()) return;
  Vec3 lo{verts[0].position[0], verts[0].position[1], verts[0].position[2]};
  Vec3 hi = lo;
  for (const asset::Vertex& v : verts) {
    const Vec3 p{v.position[0], v.position[1], v.position[2]};
    lo = Vec3{rx::Min(lo.x, p.x), rx::Min(lo.y, p.y), rx::Min(lo.z, p.z)};
    hi = Vec3{rx::Max(hi.x, p.x), rx::Max(hi.y, p.y), rx::Max(hi.z, p.z)};
  }
  const Vec3 c = (lo + hi) * 0.5f;
  mesh.bounds_center[0] = c.x;
  mesh.bounds_center[1] = c.y;
  mesh.bounds_center[2] = c.z;
  mesh.bounds_radius = Length(hi - c);
}

bool StartImGui(app::Services& s) {
  render::Renderer* renderer = s.renderer;
  gpu::Device* device = renderer ? renderer->device() : nullptr;
  if (!device || device->is_stub()) return false;
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.BackendFlags |=
      ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
  ui::ApplyRxImGuiStyle();
  base::Optional<base::Vector<u8>> ttf;
  if (s.vfs) ttf = s.vfs->Read(ui::kRxDefaultFontPath);
  if (ttf) g->font = ui::LoadRxImGuiFont(ttf->data(), ttf->size());
  if (!g->font) {
    RX_WARN("rxc: {} unavailable, the hud uses imgui's built-in font", ui::kRxDefaultFontPath);
    g->font = io.Fonts->AddFontDefault();
  }
  if (s.window) {
    auto* sdl_window = static_cast<SDL_Window*>(s.window->native_handles().window);
    if (sdl_window && ImGui_ImplSDL3_InitForVulkan(sdl_window)) {
      g->imgui_sdl = true;
      s.window->set_event_hook([](const void* event) {
        const auto* e = static_cast<const SDL_Event*>(event);
        if (e->type == SDL_EVENT_MOUSE_WHEEL) g->wheel += e->wheel.y;
        ImGui_ImplSDL3_ProcessEvent(e);
      });
    }
  }
  if (!g->imgui_renderer.Initialize(*device, renderer->swapchain_format())) {
    if (g->imgui_sdl) ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    g->imgui_sdl = false;
    return false;
  }
  g->imgui = true;
  return true;
}

void BeginImGuiFrame(float dt) {
  if (!g->imgui) return;
  if (g->imgui_sdl) {
    ImGui_ImplSDL3_NewFrame();
  } else {
    // Offscreen: no platform backend, so the display is the render target.
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(g->services->renderer->output_width()),
                            static_cast<float>(g->services->renderer->output_height()));
    io.DeltaTime = dt > 0.0f ? dt : 1.0f / 60.0f;
  }
  ImGui::NewFrame();
}

class CApp final : public app::Application {
 public:
  bool OnInitialize(app::Services& services) override {
    g->services = &services;
    g->rendering = services.renderer && services.renderer->device() &&
                   !services.renderer->device()->is_stub();
    StartImGui(services);
    return g->callbacks.init ? g->callbacks.init(g->callbacks.user) != 0 : true;
  }

  void OnUpdate(f32 dt) override {
    g->time += dt;
    if (g->imgui) {
      // A frame left open (no view was built) is dropped, not drawn.
      if (g->imgui_open) ImGui::EndFrame();
      BeginImGuiFrame(dt);
      g->imgui_open = true;
    }
    if (g->callbacks.update) g->callbacks.update(g->callbacks.user, dt);
    g->wheel = 0.0f;
  }

  void OnBuildView(f32 dt, render::FrameView& view) override {
    g->view = &view;
    g->in_frame = true;
    g->lines.clear();
    g->next_transforms.clear();
    if (g->callbacks.build_view) g->callbacks.build_view(g->callbacks.user, dt);
    view.debug_lines = base::Span<const render::DebugLine>(g->lines.data(), g->lines.size());
    base::Swap(g->prev_transforms, g->next_transforms);
    g->in_frame = false;
    g->view = nullptr;
    if (g->imgui && g->imgui_open) {
      // The draw data stays valid until the next NewFrame, which is after
      // the renderer recorded this frame's ui pass.
      ImGui::Render();
      g->imgui_open = false;
      view.ui_draw = [](gpu::CommandList& cmd) {
        g->imgui_renderer.Render(ImGui::GetDrawData(), cmd);
      };
    }
  }

  void OnSimulate(f32 dt) override {
    // Headless runs draw nothing, so the game ticks from here instead.
    if (!g->rendering) {
      g->time += dt;
      if (g->callbacks.update) g->callbacks.update(g->callbacks.user, dt);
    }
  }

  void OnShutdown() override {
    if (g->callbacks.shutdown) g->callbacks.shutdown(g->callbacks.user);
    if (g->imgui) {
      g->imgui_renderer.Shutdown();
      if (g->imgui_sdl) ImGui_ImplSDL3_Shutdown();
      ImGui::DestroyContext();
      g->imgui = false;
    }
  }
};

ImDrawList* Hud() {
  if (!g || !g->imgui || !g->imgui_open) return nullptr;
  return ImGui::GetForegroundDrawList();
}

}  // namespace
}  // namespace rx::rxc

using namespace rx;
using rx::rxc::g;

extern "C" {

int32_t rxc_run(const rxc_config* config, const rxc_callbacks* callbacks) {
  if (!config || !callbacks) return 2;
  rxc::State state;
  rxc::CApp app;
  g = &state;
  state.app = &app;
  state.callbacks = *callbacks;

  app::AppConfig cfg;
  cfg.id = config->id ? config->id : "";
  cfg.name = config->name ? config->name : "";
  cfg.title = config->title ? config->title : "";
  cfg.width = config->width;
  cfg.height = config->height;
  cfg.headless = config->headless != 0;
  cfg.offscreen = config->headless != 0 && config->offscreen != 0;
  cfg.fixed_delta = config->fixed_delta;
  cfg.splash = false;
  cfg.gather_entity_draws = false;
  cfg.renderer.enable_raytracing = config->raytracing != 0;
  static const render::QualityPreset kPresets[] = {
      render::QualityPreset::kAuto, render::QualityPreset::kLowEnd,
      render::QualityPreset::kMedium, render::QualityPreset::kHigh,
      render::QualityPreset::kUltra};
  if (config->quality >= 0 && config->quality <= 4) cfg.preset = kPresets[config->quality];
  const bool vsync = config->vsync != 0;
  cfg.tune_settings = [vsync](render::RenderSettings& s) { s.vsync = vsync; };

  app::Host host;
  state.host = &host;
  int32_t rc = 1;
  if (host.Initialize(cfg, app)) {
    rc = host.Run();
    host.Shutdown();
  }
  g = nullptr;
  return rc;
}

void rxc_quit(void) {
  if (g && g->host) g->host->RequestQuit();
}

double rxc_time(void) { return g ? g->time : 0.0; }

void rxc_get_look(rxc_look* look) {
  if (!g || !look || !g->services->renderer) return;
  const render::RenderSettings& s = g->services->renderer->settings();
  *look = rxc_look{};
  look->sun_direction[0] = s.sun_direction.x;
  look->sun_direction[1] = s.sun_direction.y;
  look->sun_direction[2] = s.sun_direction.z;
  look->sun_color[0] = s.sun_color.x;
  look->sun_color[1] = s.sun_color.y;
  look->sun_color[2] = s.sun_color.z;
  look->sun_intensity = s.sun_intensity;
  look->ambient = s.ambient;
  look->exposure = s.exposure;
  look->auto_exposure = s.auto_exposure;
  look->bloom_intensity = s.bloom ? s.bloom_intensity : 0.0f;
  look->vignette = s.vignette;
  look->film_grain = s.film_grain;
  look->chromatic_aberration = s.chromatic_aberration;
  look->sky = s.sky;
  look->interior = s.interior;
  look->interior_ambient[0] = s.interior_ambient.x;
  look->interior_ambient[1] = s.interior_ambient.y;
  look->interior_ambient[2] = s.interior_ambient.z;
  look->fog_near = s.interior_fog_near;
  look->fog_far = s.interior_fog_far;
  look->fog_color[0] = s.interior_fog_far_color.x;
  look->fog_color[1] = s.interior_fog_far_color.y;
  look->fog_color[2] = s.interior_fog_far_color.z;
  look->ssao = s.ssao;
  look->ssr = s.ssr;
  look->motion_blur = s.motion_blur;
}

void rxc_set_look(const rxc_look* look) {
  if (!g || !look || !g->services->renderer) return;
  render::RenderSettings& s = g->services->renderer->settings();
  s.sun_direction = Normalize(rxc::V3(look->sun_direction));
  s.sun_color = rxc::V3(look->sun_color);
  s.sun_intensity = look->sun_intensity;
  s.ambient = look->ambient;
  s.night = 0.0f;
  s.exposure = look->exposure;
  s.auto_exposure = look->auto_exposure != 0;
  s.bloom = look->bloom_intensity > 0.0f;
  s.bloom_intensity = look->bloom_intensity;
  s.vignette = look->vignette;
  s.film_grain = look->film_grain;
  s.chromatic_aberration = look->chromatic_aberration;
  s.sky = look->sky != 0;
  s.interior = look->interior != 0;
  s.interior_ambient = rxc::V3(look->interior_ambient);
  s.interior_fog_near = look->fog_near;
  s.interior_fog_far = look->fog_far;
  s.interior_fog_near_color = rxc::V3(look->fog_color);
  s.interior_fog_far_color = rxc::V3(look->fog_color);
  s.ssao = look->ssao != 0;
  s.ssr = look->ssr != 0;
  s.motion_blur = look->motion_blur != 0;
  s.lens_flare = 0.0f;
}

uint32_t rxc_texture_rgba8(uint32_t width, uint32_t height, const uint8_t* pixels,
                           int32_t srgb) {
  if (!g || !g->services->renderer || !pixels || width == 0 || height == 0) return 0;
  asset::Texture tex;
  tex.id = rxc::NewId("texture");
  tex.format = asset::TextureFormat::kRgba8;
  tex.width = width;
  tex.height = height;
  tex.is_srgb = srgb != 0;
  tex.data.resize(static_cast<size_t>(width) * height * 4);
  memcpy(tex.data.data(), pixels, tex.data.size());
  if (!g->services->renderer->UploadTexture(tex)) return 0;
  g->textures.push_back(tex.id);
  return static_cast<uint32_t>(g->textures.size());
}

uint32_t rxc_material(const rxc_material_desc* desc) {
  if (!g || !desc || !g->services->renderer) return 0;
  asset::Material m;
  m.id = rxc::NewId("material");
  rxc::FillMaterial(m, *desc);
  if (!g->services->renderer->UploadMaterial(m)) return 0;
  g->materials.push_back(m);
  return static_cast<uint32_t>(g->materials.size());
}

void rxc_material_update(uint32_t material, const rxc_material_desc* desc) {
  if (!g || !desc || material == 0 || material > g->materials.size()) return;
  asset::Material& m = g->materials[material - 1];
  rxc::FillMaterial(m, *desc);
  g->services->renderer->UpdateMaterial(m);
}

uint32_t rxc_mesh(const rxc_vertex* vertices, uint32_t vertex_count, const uint32_t* indices,
                  uint32_t index_count, uint32_t material, int32_t dynamic) {
  if (!g || !g->services->renderer || !vertices || !indices || vertex_count == 0 ||
      index_count == 0)
    return 0;
  auto rec = base::MakeUnique<rxc::MeshRecord>();
  asset::Mesh& mesh = rec->mesh;
  mesh.id = rxc::NewId("mesh");
  mesh.dynamic_vertices = dynamic != 0;
  mesh.lods.resize(1);
  asset::MeshLod& lod = mesh.lods[0];
  lod.indices.resize(index_count);
  memcpy(lod.indices.data(), indices, sizeof(u32) * index_count);
  rxc::FillVertices(lod, vertices, vertex_count, indices, index_count);
  if (material && material <= g->materials.size())
    lod.submeshes.push_back({0, index_count, g->materials[material - 1].id});
  rxc::ComputeBounds(mesh);
  if (!g->services->renderer->UploadMesh(mesh)) return 0;
  rec->hash = mesh.id.hash;
  if (!dynamic) {
    // Static meshes never change, so the cpu copy is dead weight.
    mesh.lods[0].vertices = {};
    mesh.lods[0].indices = {};
  }
  g->meshes.push_back(base::move(rec));
  return static_cast<uint32_t>(g->meshes.size());
}

int32_t rxc_mesh_update(uint32_t mesh, const rxc_vertex* vertices, uint32_t vertex_count) {
  if (!g || mesh == 0 || mesh > g->meshes.size() || !vertices) return 0;
  rxc::MeshRecord& rec = *g->meshes[mesh - 1];
  asset::MeshLod& lod = rec.mesh.lods[0];
  if (!rec.mesh.dynamic_vertices || vertex_count != lod.vertices.size()) return 0;
  rxc::FillVertices(lod, vertices, vertex_count, lod.indices.data(),
                    static_cast<u32>(lod.indices.size()));
  return g->services->renderer->UpdateDynamicMesh(rec.mesh) ? 1 : 0;
}

void rxc_camera(const float eye[3], const float target[3], float fov_y, int32_t cut) {
  if (!g || !g->view) return;
  g->view->camera.eye = rxc::V3(eye);
  g->view->camera.target = rxc::V3(target);
  g->view->camera.fov_y = fov_y;
  if (cut) g->view->camera_cut = true;
}

void rxc_draw(uint32_t mesh, const float transform[16], uint32_t tint_rgb, uint64_t key) {
  if (!g || !g->view || mesh == 0 || mesh > g->meshes.size()) return;
  render::DrawItem d{};
  d.mesh = g->meshes[mesh - 1]->hash;
  memcpy(d.transform.m, transform, sizeof(d.transform.m));
  d.prev_transform = d.transform;
  if (key != 0) {
    if (const Mat4* prev = g->prev_transforms.find(key)) d.prev_transform = *prev;
    g->next_transforms[key] = d.transform;
  }
  d.tint = tint_rgb & 0xffffffu;
  g->view->draws.push_back(d);
}

void rxc_add_light(const rxc_light* l) {
  if (!g || !g->view || !l) return;
  render::PointLight p{};
  p.pos_radius[0] = l->position[0];
  p.pos_radius[1] = l->position[1];
  p.pos_radius[2] = l->position[2];
  p.pos_radius[3] = l->radius;
  p.color_intensity[0] = l->color[0];
  p.color_intensity[1] = l->color[1];
  p.color_intensity[2] = l->color[2];
  p.color_intensity[3] = l->intensity;
  p.direction_type[0] = l->direction[0];
  p.direction_type[1] = l->direction[1];
  p.direction_type[2] = l->direction[2];
  p.direction_type[3] = l->type == 1 ? 1.0f : 0.0f;
  p.params[0] = l->cos_inner;
  p.params[1] = l->cos_outer;
  g->view->lights.push_back(p);
}

void rxc_particle(const float position[3], float size, const float rgba[4]) {
  if (!g || !g->view) return;
  render::ParticleInstance p{};
  memcpy(p.pos, position, sizeof(p.pos));
  memcpy(p.prev_pos, position, sizeof(p.prev_pos));
  p.size = size;
  memcpy(p.color, rgba, sizeof(p.color));
  g->view->particles.push_back(p);
}

void rxc_particles_emissive(int32_t emissive) {
  if (g && g->view) g->view->particles_emissive = emissive != 0;
}

void rxc_world_text(const float position[3], float size, uint32_t rgba, const char* text) {
  if (!g || !g->view || !text) return;
  render::WorldText t;
  t.position = rxc::V3(position);
  t.text = text;
  t.size = size;
  // The C side speaks 0xAABBGGRR like the hud; the engine wants 0xRRGGBBAA.
  t.rgba = ((rgba & 0xffu) << 24) | (((rgba >> 8) & 0xffu) << 16) |
           (((rgba >> 16) & 0xffu) << 8) | ((rgba >> 24) & 0xffu);
  g->view->world_texts.push_back(base::move(t));
}

void rxc_debug_line(const float a[3], const float b[3], uint32_t rgba) {
  if (!g || !g->view) return;
  const u32 engine_rgba = ((rgba & 0xffu) << 24) | (((rgba >> 8) & 0xffu) << 16) |
                          (((rgba >> 16) & 0xffu) << 8) | ((rgba >> 24) & 0xffu);
  g->lines.push_back(render::DebugLine{rxc::V3(a), rxc::V3(b), engine_rgba});
}

void rxc_screen_size(float* width, float* height) {
  float w = 0.0f, h = 0.0f;
  if (g && g->imgui && ImGui::GetCurrentContext()) {
    w = ImGui::GetIO().DisplaySize.x;
    h = ImGui::GetIO().DisplaySize.y;
  } else if (g && g->services && g->services->renderer) {
    w = static_cast<float>(g->services->renderer->output_width());
    h = static_cast<float>(g->services->renderer->output_height());
  }
  if (width) *width = w;
  if (height) *height = h;
}

void rxc_hud_rect(float x, float y, float w, float h, uint32_t rgba) {
  if (ImDrawList* dl = rxc::Hud()) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), rgba);
}

void rxc_hud_quad(const float xy[8], uint32_t rgba) {
  if (ImDrawList* dl = rxc::Hud())
    dl->AddQuadFilled(ImVec2(xy[0], xy[1]), ImVec2(xy[2], xy[3]), ImVec2(xy[4], xy[5]),
                      ImVec2(xy[6], xy[7]), rgba);
}

void rxc_hud_line(float x0, float y0, float x1, float y1, float thickness, uint32_t rgba) {
  if (ImDrawList* dl = rxc::Hud()) dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), rgba, thickness);
}

void rxc_hud_text(float x, float y, float size, uint32_t rgba, const char* text) {
  if (!text) return;
  if (ImDrawList* dl = rxc::Hud()) dl->AddText(g->font, size, ImVec2(x, y), rgba, text);
}

float rxc_hud_text_width(float size, const char* text) {
  if (!text || !rxc::Hud() || !g->font) return 0.0f;
  return g->font->CalcTextSizeA(size, 1e9f, 0.0f, text).x;
}

int32_t rxc_key_down(int32_t key) {
  if (!g || !g->services->window || key < 0 || key >= RXC_KEY_COUNT) return 0;
  return g->services->window->input().key(static_cast<ui::Key>(key)) ? 1 : 0;
}

int32_t rxc_key_pressed(int32_t key) {
  if (!g || !g->services->window || key < 0 || key >= RXC_KEY_COUNT) return 0;
  return g->services->window->input().key_pressed(static_cast<ui::Key>(key)) ? 1 : 0;
}

int32_t rxc_mouse_down(int32_t button) {
  if (!g || !g->services->window || button < 0 || button > 2) return 0;
  return g->services->window->input().button(static_cast<ui::MouseButton>(button)) ? 1 : 0;
}

int32_t rxc_mouse_pressed(int32_t button) {
  if (!g || !g->services->window || button < 0 || button > 2) return 0;
  return g->services->window->input().button_pressed(static_cast<ui::MouseButton>(button)) ? 1
                                                                                            : 0;
}

void rxc_mouse_delta(float* dx, float* dy) {
  float x = 0.0f, y = 0.0f;
  if (g && g->services->window) {
    x = g->services->window->input().mouse_dx;
    y = g->services->window->input().mouse_dy;
  }
  if (dx) *dx = x;
  if (dy) *dy = y;
}

float rxc_mouse_wheel(void) { return g ? g->wheel : 0.0f; }

void rxc_capture_mouse(int32_t capture) {
  if (g && g->services->window) g->services->window->SetRelativeMouseMode(capture != 0);
}

int32_t rxc_window_focused(void) {
  return g && g->services->window && g->services->window->focused() ? 1 : 0;
}

uint32_t rxc_sound(const float* samples, uint32_t frames, uint32_t channels,
                   uint32_t sample_rate) {
  if (!g || !samples || frames == 0 || channels == 0 || sample_rate == 0) return 0;
  auto clip = base::MakeUnique<audio::AudioClip>();
  clip->channels = channels;
  clip->sample_rate = sample_rate;
  clip->samples.resize(static_cast<size_t>(frames) * channels);
  memcpy(clip->samples.data(), samples, sizeof(float) * clip->samples.size());
  g->sounds.push_back(base::move(clip));
  return static_cast<uint32_t>(g->sounds.size());
}

uint32_t rxc_play(uint32_t sound, const rxc_play_params* params) {
  if (!g || !g->services->audio || sound == 0 || sound > g->sounds.size() || !params) return 0;
  audio::PlayParams p;
  p.gain = params->gain;
  p.loop = params->loop != 0;
  p.positional = params->positional != 0;
  p.position = rxc::V3(params->position);
  if (params->ref_distance > 0.0f) p.atten.ref_dist = params->ref_distance;
  if (params->max_distance > 0.0f) p.atten.max_dist = params->max_distance;
  return g->services->audio->mixer().Play(
      base::MakeUnique<rxc::SharedClipDecoder>(&*g->sounds[sound - 1]), p);
}

void rxc_voice_stop(uint32_t voice, float fade_seconds) {
  if (g && g->services->audio && voice) g->services->audio->Stop(voice, fade_seconds);
}

void rxc_voice_gain(uint32_t voice, float gain) {
  if (g && g->services->audio && voice) g->services->audio->SetVoiceGain(voice, gain);
}

void rxc_voice_position(uint32_t voice, const float position[3]) {
  if (g && g->services->audio && voice)
    g->services->audio->SetVoicePosition(voice, rxc::V3(position));
}

void rxc_master_volume(float volume) {
  if (g && g->services->audio) g->services->audio->SetMasterVolume(volume);
}

void rxc_screenshot(const char* path) {
  if (g && g->services->renderer && path) g->services->renderer->CaptureScreenshot(path);
}

}  // extern "C"
