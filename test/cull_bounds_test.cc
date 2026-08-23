// GPU acceptance test for the bounding radius the cull shader derives from a
// model matrix. RxAxisScales (shaders/model_transform.hlsli) reads the matrix
// by COLUMN, because the columns of a rotation * scale are the images of the
// object-space axes and their lengths are exactly the per-axis scales. The rows
// are mixtures of all three and under-report the largest, so reading rows
// shrinks the bounding sphere and frustum-culls geometry that is on screen.
//
// The two readings coincide for any uniform scale, which is every instance the
// scene tree ships, so no rendered frame can tell them apart. This test builds
// the one case that can: a 45-degree-rotated diag(4, 0.25, 0.25) instance
// straddling a frustum plane at a distance the column radius clears and the row
// radius does not.
//
// Runs cull.cs through the RHI on a surfaceless device (backend from RX_RHI,
// default vulkan). Skips cleanly (exit 0) when no driver is present; run under
// vkrun for the real GPU path.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "core/math.h"
#include "render/pipeline/gpu_cull.h"
#include "render/rhi/command_list.h"
#include "render/rhi/device.h"
#include "shaders/cull_cs_hlsl.h"

using namespace rx;
using namespace rx::render;

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
  if (condition) return;
  std::fprintf(stderr, "cull_bounds_test: FAIL: %s\n", message);
  ++failures;
}

int Fail(const char* message) {
  std::fprintf(stderr, "cull_bounds_test: FAIL: %s\n", message);
  return 1;
}

// Mirrors CullPush in gpu_cull.cc / PushData in cull.cs.
struct CullPush {
  f32 planes[5][4];
  f32 eye_pad[4];
  f32 proj_hiz[4];
  u32 misc[4];  // instance_count, frustum_enabled, occlusion_enabled, pad
};

// rotation * scale, then a translation along x. Scale applied on the right (the
// 3x3 columns) is what makes the columns per-axis and the rows mixtures; a
// scale * rotation would leave the rows per-axis and hide the whole question.
Mat4 RotatedScale(f32 radians, const Vec3& scale, f32 tx) {
  Mat4 m = MakeFromQuat(QuatFromAxisAngle({0, 0, 1}, radians));
  for (int row = 0; row < 3; ++row) {
    m.m[0 + row] *= scale.x;
    m.m[4 + row] *= scale.y;
    m.m[8 + row] *= scale.z;
  }
  m.m[12] = tx;
  return m;
}

}  // namespace

int main() {
  DeviceDesc desc;
  const char* rhi = std::getenv("RX_RHI");
  desc.backend = (rhi && std::strcmp(rhi, "d3d12") == 0) ? Backend::kD3D12 : Backend::kVulkan;
  desc.enable_validation = std::getenv("RX_VALIDATION") != nullptr;
  std::unique_ptr<Device> device = Device::CreateOffscreen(desc);
  if (!device) return Fail("CreateOffscreen returned null");
  if (device->is_stub()) {
    std::printf("cull_bounds_test: no %s driver, skipping (null backend)\n",
                BackendName(desc.backend));
    return 0;
  }
  std::printf("cull_bounds_test: device '%s'\n", device->caps().adapter_name.c_str());

  // Same set layout as GpuCull::Initialize.
  PipelineHandle pipeline = device->CreateComputePipeline({
      .shader = RX_SHADER(k_cull_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageBuffer},
                          {1, BindingType::kStorageBuffer},
                          {2, BindingType::kStorageBuffer},
                          {3, BindingType::kSampledImage},
                          {4, BindingType::kUniformBuffer}}}},
      .push_constant_size = PushSize<CullPush>(),
      .debug_name = "cull_bounds_test",
  });
  if (!pipeline) return Fail("cull pipeline creation failed");

  // A 45-degree rotation of diag(4, 0.25, 0.25): column norms are (4, 0.25,
  // 0.25); the rows come out sqrt(8.03) = 2.834 and 0.25. A unit-radius mesh
  // 3.4 units outside a plane therefore survives on the column radius and dies
  // on the row one.
  constexpr f32 kQuarterTurn = 0.7853981634f;
  const Vec3 kSquash{4.0f, 0.25f, 0.25f};
  const u32 kInstances = 3;

  GpuBuffer instances = device->CreateBuffer(kInstances * sizeof(GpuCull::Instance),
                                             kBufferUsageStorage, true);
  GpuBuffer commands = device->CreateBuffer(kInstances * sizeof(GpuCull::Command),
                                            kBufferUsageStorage, true);
  GpuBuffer counts = device->CreateBuffer(16, kBufferUsageStorage, true);
  GpuBuffer reproject = device->CreateBuffer(sizeof(Mat4), kBufferUsageUniform, true);
  if (!instances.mapped || !commands.mapped || !counts.mapped || !reproject.mapped) {
    return Fail("host-visible buffer creation failed");
  }
  // The occlusion test is off, but the descriptor still has to be a live image
  // or the dispatch reads an unwritten binding.
  GpuImage hiz = device->CreateImage2D(Format::kR32Float, {1, 1}, kTextureUsageSampled);
  device->ImmediateSubmit([&](CommandList& cmd) {
    cmd.Barrier(Transition(hiz, ResourceState::kUndefined, ResourceState::kShaderReadAll));
  });

  GpuCull::Instance* inst = static_cast<GpuCull::Instance*>(instances.mapped);
  for (u32 i = 0; i < kInstances; ++i) {
    inst[i] = {};
    inst[i].bounds[3] = 1.0f;  // unit model-space sphere at the origin
    inst[i].first_cmd = i;
    inst[i].cmd_count = 1;
  }
  // Keep: 3.4 out, radius 4 on the columns (2.834 on the rows would cull it).
  inst[0].model = RotatedScale(kQuarterTurn, kSquash, 3.4f);
  // Cull: same transform 5.0 out, past even the column radius. Proves the plane
  // under test can reject at all, so instance 0 passing is not a free pass.
  inst[1].model = RotatedScale(kQuarterTurn, kSquash, 5.0f);
  // Keep: plain unit instance just outside the plane.
  inst[2].model = MakeTranslation({0.5f, 0.0f, 0.0f});

  GpuCull::Command* cmd_data = static_cast<GpuCull::Command*>(commands.mapped);
  for (u32 i = 0; i < kInstances; ++i) cmd_data[i] = {.index_count = 3, .instance_count = 1};
  *static_cast<u32*>(counts.mapped) = 0;
  const Mat4 identity = Mat4::Identity();
  std::memcpy(reproject.mapped, &identity, sizeof(identity));

  CullPush push{};
  // Plane 0 is the one under test: inside is x <= 0, so an instance at x = t
  // sits t outside it. The rest are pushed far enough out to always pass.
  const f32 planes[5][4] = {{-1, 0, 0, 0},
                            {1, 0, 0, 1000},
                            {0, 1, 0, 1000},
                            {0, -1, 0, 1000},
                            {0, 0, 1, 1000}};
  std::memcpy(push.planes, planes, sizeof(planes));
  push.misc[0] = kInstances;
  push.misc[1] = 1;  // frustum on
  push.misc[2] = 0;  // occlusion off: no hi-z to bind

  device->ImmediateSubmit([&](CommandList& cmd) {
    cmd.BindPipeline(pipeline);
    cmd.BindTransient(0, {Bind::StorageBuffer(0, instances), Bind::StorageBuffer(1, commands),
                          Bind::StorageBuffer(2, counts), Bind::Sampled(3, hiz),
                          Bind::Uniform(4, reproject, 0, sizeof(Mat4))});
    cmd.Push(push);
    cmd.Dispatch(1, 1, 1);
  });
  device->WaitIdle();

  const u32 kept0 = cmd_data[0].instance_count;
  const u32 kept1 = cmd_data[1].instance_count;
  const u32 kept2 = cmd_data[2].instance_count;
  std::printf("cull_bounds_test: instanceCount = {%u, %u, %u}, visible = %u\n", kept0, kept1,
              kept2, *static_cast<const u32*>(counts.mapped));

  Check(kept0 == 1, "rotated non-uniform instance kept (column radius 4 clears the 3.4 plane)");
  Check(kept1 == 0, "the same instance 5.0 out is culled (the plane really rejects)");
  Check(kept2 == 1, "unit instance 0.5 out is kept");
  Check(*static_cast<const u32*>(counts.mapped) == 2, "visible counter matches the kept draws");

  device->DestroyPipeline(pipeline);
  device->DestroyImage(hiz);
  device->DestroyBuffer(instances);
  device->DestroyBuffer(commands);
  device->DestroyBuffer(counts);
  device->DestroyBuffer(reproject);

  if (failures == 0) std::printf("cull_bounds_test: all checks passed\n");
  return failures;
}
