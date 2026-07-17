// Offscreen GPU acceptance test for the heightfield fluid solver. Builds a 64^2
// bowl bed with a central ridge, fills the left half with water resting against
// the ridge (so it starts in equilibrium there), steps the solver, and checks:
//   (1) total water volume conserved within 1% of the initial fill,
//   (2) no NaNs and no negative depths anywhere,
//   (3) after removing the ridge (bed re-upload with a version bump) and
//       flooding the empty half, the water surface (bed + depth) over the wet
//       cells settles level to within a few cm — the dam-break settling test.
//
// Skips cleanly (exit 0) when no Vulkan driver is present (null backend), like
// offscreen_test; run under vkrun to exercise the real GPU path.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "render/core/render_graph.h"
#include "render/geometry/fluid_sim.h"
#include "render/rhi/device.h"

using namespace rx::render;

namespace {

int Fail(const char* msg) {
  std::fprintf(stderr, "fluid_sim_test: FAIL: %s\n", msg);
  return 1;
}

constexpr u32 kRes = 64;
constexpr f32 kExtent = 64.0f;  // l = 1 m per cell
constexpr f32 kWaterLevel = 3.0f;

f32 BowlBed(u32 x, u32 y) {
  const f32 bx = static_cast<f32>(x) - 32.0f;
  const f32 by = static_cast<f32>(y) - 32.0f;
  return 0.02f * (bx * bx + by * by);  // center 0, edges high (contains water)
}

}  // namespace

int main() {
  DeviceDesc desc;
  desc.request_raytracing = false;
  std::unique_ptr<Device> device = Device::CreateOffscreen(desc);
  if (!device) return Fail("CreateOffscreen returned null");
  if (device->is_stub()) {
    std::printf("fluid_sim_test: no Vulkan driver, skipping (null backend)\n");
    return 0;
  }
  std::printf("fluid_sim_test: device '%s'\n", device->caps().adapter_name.c_str());

  // Bed with the ridge (a dam down the middle) and bed without it (dam removed).
  std::vector<f32> bed_ridge(static_cast<size_t>(kRes) * kRes);
  std::vector<f32> bed_open(static_cast<size_t>(kRes) * kRes);
  std::vector<f32> water0(static_cast<size_t>(kRes) * kRes, 0.0f);
  f32 initial_volume = 0.0f;
  for (u32 y = 0; y < kRes; ++y) {
    for (u32 x = 0; x < kRes; ++x) {
      const u32 i = y * kRes + x;
      const f32 bowl = BowlBed(x, y);
      bed_open[i] = bowl;
      const bool ridge = (x >= 31 && x <= 33);
      bed_ridge[i] = bowl + (ridge ? 8.0f : 0.0f);
      // Fill the left basin up to a flat surface resting against the ridge, so
      // there is no steep free face at t=0 (only the smooth bowl waterline).
      if (x <= 30) {
        const f32 depth = kWaterLevel - bowl;
        if (depth > 0.0f) {
          water0[i] = depth;
          initial_volume += depth;  // l^2 = 1
        }
      }
    }
  }

  FluidSim sim;
  if (!sim.Initialize(*device)) return Fail("FluidSim::Initialize failed");

  FluidDomainDesc dom;
  dom.origin[0] = 0.0f;
  dom.origin[1] = 0.0f;
  dom.extent = kExtent;
  dom.resolution = kRes;
  dom.bed = bed_ridge.data();
  dom.initial_water = water0.data();
  dom.bed_version = 1;
  dom.ambient_temperature = 20.0f;

  RenderGraph graph;             // reset before each frame; device passed to Compile
  TransientPool pool(*device);   // caches the transient images across frames

  // Configure (allocate images + upload bed/water) outside the frame loop with a
  // zero-dt step: no substeps run, but Configure fires its ImmediateSubmit.
  auto configure = [&](const FluidDomainDesc& d) {
    RenderGraph g;
    FluidSim::UpdateParams up;
    up.domain = &d;
    up.dt = 0.0f;
    up.frame_slot = 0;
    sim.AddToGraph(g, up);
  };
  configure(dom);

  // Steps the solver for `frames` frames at 1/30 s (4 substeps/frame).
  u32 frame_index = 0;
  auto run = [&](const FluidDomainDesc& d, u32 frames) -> bool {
    for (u32 f = 0; f < frames; ++f) {
      const u32 slot = frame_index++ % Device::kMaxFramesInFlight;
      CommandList* cmd = device->BeginFrame(slot);
      if (!cmd) return false;
      pool.BeginFrame();
      graph.Reset();
      FluidSim::UpdateParams up;
      up.domain = &d;
      up.dt = 1.0f / 30.0f;
      up.frame_slot = slot;
      sim.AddToGraph(graph, up);
      if (!graph.Compile(*device, pool)) return false;
      PassContext ctx;
      ctx.cmd = cmd;
      ctx.device = device.get();
      ctx.graph = &graph;
      graph.Execute(ctx);
      device->SubmitFrame(cmd);
    }
    device->WaitIdle();
    return true;
  };

  auto readback = [&](std::vector<f32>& out) -> bool {
    out.assign(static_cast<size_t>(kRes) * kRes * 4, 0.0f);
    const GpuImage& state = FluidSimProbe::state(sim);
    return device->ReadbackImage(state, ResourceState::kGeneral, out.data(),
                                 out.size() * sizeof(f32));
  };

  // Verify the upload landed before stepping (bed + water, flat pond).
  {
    std::vector<f32> s0;
    if (!readback(s0)) return Fail("ReadbackImage failed (pre-step)");
    f32 v = 0;
    for (u32 i = 0; i < kRes * kRes; ++i) v += s0[i * 4 + 0];
    std::printf("fluid_sim_test: uploaded water volume %.3f (cpu %.3f)\n", v, initial_volume);
    if (std::fabs(v - initial_volume) > 0.01f * initial_volume)
      return Fail("uploaded water volume does not match the CPU fill");
  }

  // --- Phase 1: settle behind the ridge, then check conservation + sanity ---
  if (!run(dom, 75)) return Fail("frame loop failed (phase 1)");  // ~300 substeps

  std::vector<f32> s1;
  if (!readback(s1)) return Fail("ReadbackImage failed (phase 1)");

  f32 vol1 = 0.0f;
  for (u32 i = 0; i < kRes * kRes; ++i) {
    const f32 dw = s1[i * 4 + 0];
    const f32 dl = s1[i * 4 + 1];
    const f32 T = s1[i * 4 + 2];
    const f32 C = s1[i * 4 + 3];
    if (!std::isfinite(dw) || !std::isfinite(dl) || !std::isfinite(T) || !std::isfinite(C))
      return Fail("non-finite value in state");
    if (dw < -1e-4f || dl < -1e-4f || C < -1e-4f) return Fail("negative depth/crust in state");
    vol1 += dw;
  }
  const f32 drift = std::fabs(vol1 - initial_volume) / std::max(initial_volume, 1e-6f);
  std::printf("fluid_sim_test: initial volume %.3f, after settle %.3f (drift %.3f%%)\n",
              initial_volume, vol1, drift * 100.0f);
  if (drift > 0.01f) return Fail("water volume not conserved within 1%");

  // --- Phase 2: remove the ridge (bed re-upload) and flood the empty half ----
  dom.bed = bed_open.data();
  dom.initial_water = nullptr;  // no re-seed; only the bed changes
  dom.bed_version = 2;
  configure(dom);  // reconfigure? no: same resolution/extent/origin -> bed re-upload only

  // The flood is a low-dissipation slosh: it decays toward a level plane over
  // seconds of sim time (water drag alone). Step in chunks until the core pond
  // is level (or a generous cap), measuring the surface over clearly-wet cells
  // so the flickering shoreline does not dominate the spread.
  constexpr f32 kLevelTol = 0.05f;  // "a few cm"
  constexpr u32 kMaxChunks = 40;    // 40 * 100 frames = 16000 substeps cap
  std::vector<f32> s2;
  f32 spread = 1e9f;
  u32 wet = 0;
  for (u32 chunk = 0; chunk < kMaxChunks && spread > kLevelTol; ++chunk) {
    if (!run(dom, 100)) return Fail("frame loop failed (phase 2)");
    if (!readback(s2)) return Fail("ReadbackImage failed (phase 2)");
    f32 mn = 1e9f, mx = -1e9f, vol2 = 0.0f;
    wet = 0;
    for (u32 i = 0; i < kRes * kRes; ++i) {
      const f32 dw = s2[i * 4 + 0];
      if (!std::isfinite(dw)) return Fail("non-finite depth after flood");
      if (dw < -1e-4f) return Fail("negative depth after flood");
      vol2 += dw;
      if (dw > 0.5f) {  // core pond; shoreline partial cells sit below the plane
        const f32 surface = bed_open[i] + dw;
        mn = std::fmin(mn, surface);
        mx = std::fmax(mx, surface);
        ++wet;
      }
    }
    spread = (wet > 0) ? mx - mn : 1e9f;
    std::printf("fluid_sim_test: flood %u frames, volume %.3f, wet %u, spread %.3f m\n",
                (chunk + 1) * 100, vol2, wet, spread);
  }
  if (wet < 16) return Fail("too few wet cells after flood (sim collapsed?)");
  if (spread > kLevelTol) return Fail("flooded surface not level within tolerance");

  sim.Destroy(*device);
  std::printf("fluid_sim_test: PASS\n");
  return 0;
}
