#include <cstdio>

#include "asset/material.h"
#include "asset/mesh.h"
#include "render/core/renderer.h"

using namespace rx;
using namespace rx::render;

int main() {
  Renderer renderer;
  RendererDesc desc;
  desc.backend = Backend::kVulkan;
  desc.enable_validation = true;
  if (!renderer.InitializeOffscreen(desc, 32, 32)) return 1;
  if (!renderer.caps() || !renderer.caps()->ray_query) {
    std::printf("path_material_test: SKIP, ray queries unavailable\n");
    return 77;
  }
  RenderSettings& settings = renderer.settings();
  settings.path_trace = true;
  settings.path_trace_reference = true;
  settings.path_trace_recon = false;
  settings.fog = false;
  settings.clouds = false;
  settings.auto_exposure = false;
  settings.upscaler = UpscalerKind::kNone;
  asset::Material material;
  material.id = asset::MakeAssetId("test/path_material");
  material.emissive_factor[0] = 1;
  if (!renderer.UploadMaterial(material)) return 1;
  FrameView view;
  view.frame_delta_seconds = 0;
  renderer.RenderFrame(view);
  renderer.RenderFrame(view);
  const u32 accumulated = renderer.path_trace_samples();
  renderer.WaitIdle();
  material.emissive_factor[0] = 0;
  if (!renderer.UpdateMaterial(material)) return 1;
  renderer.RenderFrame(view);
  const u32 restarted = renderer.path_trace_samples();
  renderer.RenderFrame(view);
  const u32 resumed = renderer.path_trace_samples();
  material.id = asset::MakeAssetId("test/missing_material");
  const bool missing_updated = renderer.UpdateMaterial(material);
  renderer.RenderFrame(view);
  const u32 after_missing = renderer.path_trace_samples();
  asset::Mesh mesh;
  mesh.id = asset::MakeAssetId("test/path_motion_mesh");
  mesh.lods.resize(1);
  mesh.lods[0].vertices = {{.position = {-1, -1, 0}, .normal = {0, 0, -1}},
                    {.position = {1, -1, 0}, .normal = {0, 0, -1}},
                    {.position = {0, 1, 0}, .normal = {0, 0, -1}}};
  mesh.lods[0].indices = {0, 1, 2};
  mesh.lods[0].submeshes.push_back({.index_count = 3});
  if (!renderer.UploadMesh(mesh)) return 1;
  view.draws.push_back({.mesh = mesh.id.hash});
  renderer.RenderFrame(view);
  renderer.RenderFrame(view);
  const u32 static_samples = renderer.path_trace_samples();
  view.draws[0].transform = MakeTranslation({1, 0, 0});
  renderer.RenderFrame(view);
  const u32 moving_samples = renderer.path_trace_samples();
  view.draws[0].prev_transform = view.draws[0].transform;
  renderer.RenderFrame(view);
  const u32 stopped_samples = renderer.path_trace_samples();
  view.camera_cut = true;
  renderer.RenderFrame(view);
  const u32 cut_samples = renderer.path_trace_samples();
  std::printf("rigid motion: static=%u moving=%u stopped=%u cut=%u\n",
               static_samples, moving_samples, stopped_samples, cut_samples);
  renderer.Shutdown();
  const bool ok = accumulated > 0 && restarted > 0 && restarted < accumulated &&
                  resumed == 2 * restarted && !missing_updated && after_missing == 3 * restarted;
  std::printf("path_material_test: accumulated=%u restarted=%u resumed=%u after_missing=%u\n",
               accumulated, restarted, resumed, after_missing);
  return ok && static_samples == 2 * moving_samples && stopped_samples == static_samples &&
         cut_samples == moving_samples ? 0 : 1;
}
