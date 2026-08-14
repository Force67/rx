// Texture-space decal baking, end to end on a real GPU: stamp a projector onto
// a quad, bake it into the receiver's tile and read the atlas back. Then evict
// the tile out from under the receiver and prove the CPU-side journal rebakes
// it - that replay is what lets the system keep unlimited decals in bounded
// memory, so it is the part worth a test.
//
// Skips cleanly (exit 0) when no Vulkan driver is present, like offscreen_test;
// the projector math at the top is pure CPU and always runs. Run under vkrun to
// exercise the GPU path.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "asset/mesh.h"
#include "render/core/render_graph.h"
#include "render/rhi/command_list.h"
#include "render/rhi/device.h"
#include "render/texturing/decal_bake.h"

using namespace rx::render;
using rx::asset::Vertex;
using rx::Vec3;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
  if (ok) return;
  std::fprintf(stderr, "decal_bake_test: FAIL: %s\n", what);
  ++failures;
}

Vec3 ToDecalSpace(const Decal& d, const Vec3& world) {
  return {d.row0[0] * world.x + d.row0[1] * world.y + d.row0[2] * world.z + d.row0[3],
          d.row1[0] * world.x + d.row1[1] * world.y + d.row1[2] * world.z + d.row1[3],
          d.row2[0] * world.x + d.row2[1] * world.y + d.row2[2] * world.z + d.row2[3]};
}

bool Near(f32 a, f32 b) { return std::fabs(a - b) < 1e-4f; }

// A unit quad in the XZ plane, uv0 covering the full 0..1 chart. World x maps to
// u and world z to v, so a projector at the origin lands in the middle of the
// tile and the tile corners stay outside it.
GpuMesh CreateQuad(Device& device, f32 udim_u = 0) {
  const f32 u0 = udim_u;
  const f32 u1 = udim_u + 1.0f;
  const Vertex vertices[4] = {
      {{-1, 0, -1}, {0, 1, 0}, {1, 0, 0, 1}, {u0, 0}, 0xffffffff},
      {{1, 0, -1}, {0, 1, 0}, {1, 0, 0, 1}, {u1, 0}, 0xffffffff},
      {{1, 0, 1}, {0, 1, 0}, {1, 0, 0, 1}, {u1, 1}, 0xffffffff},
      {{-1, 0, 1}, {0, 1, 0}, {1, 0, 0, 1}, {u0, 1}, 0xffffffff},
  };
  const u32 indices[6] = {0, 1, 2, 0, 2, 3};
  GpuMesh mesh;
  mesh.vertices = device.CreateBufferWithData(
      {reinterpret_cast<const u8*>(vertices), sizeof(vertices)}, kBufferUsageVertex);
  mesh.indices = device.CreateBufferWithData(
      {reinterpret_cast<const u8*>(indices), sizeof(indices)}, kBufferUsageIndex);
  mesh.index_count = 6;
  mesh.vertex_count = 4;
  return mesh;
}

// 1x1 opaque white: the "authored" decal page every stamp in this test samples.
GpuImage CreateWhiteSource(Device& device) {
  GpuImage image = device.CreateImage2D(Format::kRGBA8Unorm, {1, 1},
                                        kTextureUsageSampled | kTextureUsageTransferDst);
  if (!image) return image;
  const f32 white[4] = {1, 1, 1, 1};
  device.ImmediateSubmit([&](CommandList& cmd) {
    cmd.Barrier(Transition(image, ResourceState::kUndefined, ResourceState::kCopyDst));
    cmd.ClearColor(image, white);
    cmd.Barrier(Transition(image, ResourceState::kCopyDst, ResourceState::kShaderReadFragment));
  });
  return image;
}

void RunBake(Device& device, DecalBaker& baker, TransientPool& pool,
             const DecalBaker::Target& target, u64 frame_index, TextureView source) {
  RenderGraph graph;
  pool.BeginFrame();
  baker.AddToGraph(graph, {&target, 1}, 0, frame_index, source, source);
  CommandList* cmd = device.BeginFrame(0);
  if (!cmd || !graph.Compile(device, pool)) {
    Check(false, "frame and graph setup");
    return;
  }
  PassContext ctx;
  ctx.cmd = cmd;
  ctx.device = &device;
  device.SubmitFrame(graph.Execute(ctx));
  device.WaitIdle();
}

}  // namespace

int main() {
  // --- projector math (no GPU) ---
  {
    const Decal d = MakeDecalProjector({2, 3, 4}, {0, 1, 0}, {0, 0, 1}, 2.0f, 2.0f, 1.0f);
    const Vec3 center = ToDecalSpace(d, {2, 3, 4});
    Check(Near(center.x, 0) && Near(center.y, 0) && Near(center.z, 0),
          "the projector centre is the origin of decal space");
    // Half the FULL width along a plane axis is the box edge.
    const Vec3 edge = ToDecalSpace(d, {2, 3, 5});
    Check(Near(std::fabs(edge.x) + std::fabs(edge.y), 1.0f) && Near(edge.z, 0),
          "a point at half the width sits on the box edge");
    const Vec3 above = ToDecalSpace(d, {2, 3.5f, 4});
    Check(Near(above.z, 1.0f), "the box depth runs along the surface normal");
    const Vec3 outside = ToDecalSpace(d, {5, 3, 4});
    Check(std::fabs(outside.x) > 1.0f, "a point past the box falls outside");
  }

  DeviceDesc desc;
  desc.backend = Backend::kVulkan;
  desc.request_raytracing = false;
  // RX_VALIDATION=1 runs the bake through the Vulkan validation layers, which
  // is where the pass's hand-written image barriers get checked.
  desc.enable_validation = std::getenv("RX_VALIDATION") != nullptr;
  std::unique_ptr<Device> device = Device::CreateOffscreen(desc);
  if (!device) {
    std::fprintf(stderr, "decal_bake_test: FAIL: CreateOffscreen returned null\n");
    return 1;
  }
  if (device->is_stub()) {
    std::printf("decal_bake_test: no vulkan driver, skipping the gpu half\n");
    return failures == 0 ? 0 : 1;
  }
  std::printf("decal_bake_test: device '%s'\n", device->caps().adapter_name.c_str());

  // One tile only: the second receiver has to evict the first, which is exactly
  // the path the journal exists for.
  DecalBaker baker;
  DecalBaker::Desc bd;
  bd.atlas_size = 64;
  bd.tile_size = 64;
  if (!baker.Initialize(*device, bd)) {
    std::fprintf(stderr, "decal_bake_test: FAIL: baker initialize\n");
    return 1;
  }
  Check(baker.stats().tile_capacity == 1, "a 64/64 atlas holds exactly one tile");

  GpuMesh quad = CreateQuad(*device);
  GpuImage source = CreateWhiteSource(*device);
  if (!quad.vertices || !quad.indices || !source) {
    std::fprintf(stderr, "decal_bake_test: FAIL: test resource creation\n");
    return 1;
  }
  TransientPool pool(*device);

  const u32 first = baker.AcquireReceiver();
  const u32 second = baker.AcquireReceiver();
  Check(first != 0 && second != 0 && first != second, "receivers get distinct handles");
  Check(baker.tile_slot(first) == 0, "a receiver holds no tile before its first stamp");

  std::vector<u8> pixels(static_cast<size_t>(bd.atlas_size) * bd.atlas_size * 4);
  auto coverage_at = [&](u32 x, u32 y) -> u32 {
    return pixels[(static_cast<size_t>(y) * bd.atlas_size + x) * 4 + 3];
  };
  auto read_atlas = [&] {
    std::memset(pixels.data(), 0, pixels.size());
    Check(device->ReadbackImage(baker.albedo_atlas(), ResourceState::kShaderReadFragment,
                                pixels.data(), pixels.size()),
          "reading the layer atlas back");
    // ReadbackImage leaves the image in kCopySrc, but the baker rightly assumes
    // it owns the atlas and left it shader-readable. Nothing in the renderer
    // reads the atlas back; put it where the baker expects it so the next bake's
    // barriers stay honest (and validation stays quiet).
    device->ImmediateSubmit([&](CommandList& cmd) {
      cmd.Barrier(Transition(baker.albedo_atlas(), ResourceState::kCopySrc,
                             ResourceState::kShaderReadFragment));
    });
  };

  DecalBaker::Target target;
  target.mesh = &quad;

  // A projector one unit across, centred on the quad: world x/z in [-0.5, 0.5],
  // which is uv 0.25..0.75, so the tile centre is covered and its corners are not.
  DecalStamp stamp;
  stamp.receiver = first;
  stamp.projector = MakeDecalProjector({0, 0, 0}, {0, 1, 0}, {0, 0, 1}, 1.0f, 1.0f, 1.0f);
  Check(baker.Stamp(stamp), "stamping a live receiver is accepted");
  Check(!baker.Stamp(DecalStamp{}), "stamping receiver 0 is rejected");

  target.receiver = first;
  RunBake(*device, baker, pool, target, 1, source.view);
  Check(baker.tile_slot(first) == 1, "the stamped receiver got the only tile");
  Check(baker.stats().resident_tiles == 1, "one tile is resident");

  read_atlas();
  const u32 mid = bd.tile_size / 2;
  std::printf("decal_bake_test: centre coverage %u, corner coverage %u\n", coverage_at(mid, mid),
              coverage_at(2, 2));
  Check(coverage_at(mid, mid) > 240, "the projector covers the middle of the tile");
  Check(coverage_at(2, 2) == 0, "the tile corner is outside the projector");

  // --- eviction and journal rebake ---
  DecalStamp other = stamp;
  other.receiver = second;
  Check(baker.Stamp(other), "the second receiver takes a stamp");
  target.receiver = second;
  RunBake(*device, baker, pool, target, 2, source.view);
  Check(baker.tile_slot(second) == 1, "the second receiver took the tile over");
  Check(baker.tile_slot(first) == 0, "the evicted receiver holds no tile");
  Check(baker.stats().evictions == 1, "the tile hand-over is counted");

  // Nothing new was stamped on `first`: everything it gets back comes from the
  // journal it kept while its pixels were gone.
  target.receiver = first;
  RunBake(*device, baker, pool, target, 3, source.view);
  Check(baker.tile_slot(first) == 1, "the returning receiver reclaimed the tile");
  read_atlas();
  std::printf("decal_bake_test: rebaked centre coverage %u\n", coverage_at(mid, mid));
  Check(coverage_at(mid, mid) > 240, "the journal replayed the decal into the fresh tile");
  Check(coverage_at(2, 2) == 0, "the reclaimed tile was cleared of the other receiver");

  // A wash-off drops the history, so the next bake leaves a clean tile.
  baker.ClearReceiver(first);
  RunBake(*device, baker, pool, target, 4, source.view);
  read_atlas();
  Check(coverage_at(mid, mid) == 0, "clearing a receiver repaints its tile empty");

  baker.ReleaseReceiver(first);
  baker.ReleaseReceiver(second);
  Check(baker.stats().receivers == 0, "released receivers are gone");

  // --- UDIM: a receiver whose uvs live on tile 2 ---
  // Real character bodies (Daz/Genesis) lay their zones out across u in [0,7).
  // Without a bias the whole mesh sits outside 0..1 and nothing may bake; with
  // one, the addressed zone gets the entire layer.
  GpuMesh udim_quad = CreateQuad(*device, 2.0f);
  const u32 shifted = baker.AcquireReceiver();
  target.receiver = shifted;
  target.mesh = &udim_quad;
  stamp.receiver = shifted;
  Check(baker.Stamp(stamp), "the udim receiver takes a stamp");
  RunBake(*device, baker, pool, target, 5, source.view);
  read_atlas();
  Check(coverage_at(mid, mid) == 0, "un-biased uvs off the 0..1 square bake nothing");

  baker.SetReceiverUv(shifted, 1.0f, 1.0f, -2.0f, 0.0f);
  RunBake(*device, baker, pool, target, 6, source.view);
  read_atlas();
  std::printf("decal_bake_test: udim centre coverage %u\n", coverage_at(mid, mid));
  Check(coverage_at(mid, mid) > 240, "biasing onto the uv tile bakes the decal");
  Check(coverage_at(2, 2) == 0, "the tile corner is still outside the projector");
  baker.ReleaseReceiver(shifted);
  device->DestroyBuffer(udim_quad.vertices);
  device->DestroyBuffer(udim_quad.indices);

  baker.Destroy(*device);
  device->DestroyImage(source);
  device->DestroyBuffer(quad.vertices);
  device->DestroyBuffer(quad.indices);

  std::printf("decal_bake_test: %s\n", failures == 0 ? "PASS" : "FAIL");
  return failures == 0 ? 0 : 1;
}
