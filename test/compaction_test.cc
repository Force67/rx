// Acceleration-structure compaction acceptance test. Through the RHI on a
// surfaceless Vulkan device: build a small triangle BLAS with allow_compaction,
// query its compacted size across a submit boundary (exercising both the
// non-blocking poll and the post-fence read), create a tight BLAS at that size,
// compact-copy into it, and (when ray queries are available) build a TLAS over
// the compacted BLAS to prove it is still a valid, addressable structure. The
// fat BLAS is retired through the frame-safe graveyard.
//
// Skips cleanly (exit 0) when no Vulkan driver is present (null backend) or the
// adapter has no ray tracing. Run under vkrun to exercise the real GPU path.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "render/rhi/command_list.h"
#include "render/rhi/device.h"

using namespace rx::render;

namespace {

int Fail(const char* msg) {
  std::fprintf(stderr, "compaction_test: FAIL: %s\n", msg);
  return 1;
}

u64 AlignUp(u64 v, u64 a) { return (v + a - 1) & ~(a - 1); }

}  // namespace

int main() {
  DeviceDesc desc;
  desc.backend = Backend::kVulkan;
  desc.request_raytracing = true;
  desc.enable_validation = std::getenv("RX_VALIDATION") != nullptr;
  std::unique_ptr<Device> device = Device::CreateOffscreen(desc);
  if (!device) return Fail("CreateOffscreen returned null");

  if (device->is_stub()) {
    std::printf("compaction_test: no vulkan driver, skipping (null backend)\n");
    return 0;
  }
  if (!device->caps().raytracing) {
    std::printf("compaction_test: adapter '%s' has no ray tracing, skipping\n",
                device->caps().adapter_name.c_str());
    return 0;
  }
  std::printf("compaction_test: device '%s'\n", device->caps().adapter_name.c_str());

  // --- a single opaque triangle in a host-visible, AS-build-input buffer ---
  const f32 verts[9] = {-0.5f, -0.5f, 0.0f, 0.5f, -0.5f, 0.0f, 0.0f, 0.5f, 0.0f};
  GpuBuffer vbo = device->CreateBuffer(sizeof(verts),
                                       kBufferUsageAccelBuildInput | kBufferUsageDeviceAddress,
                                       /*host_visible=*/true);
  if (!vbo || !vbo.mapped || vbo.address == 0) return Fail("vertex buffer creation failed");
  std::memcpy(vbo.mapped, verts, sizeof(verts));

  AccelTriangles tri{.vertex_address = vbo.address,
                     .vertex_stride = 3 * sizeof(f32),
                     .vertex_count = 3,
                     .vertex_format = Format::kRGB32Float,
                     .index_count = 0,  // non-indexed soup
                     .opaque = true};
  BlasBuildDesc build_desc{.geometries = {&tri, 1}, .fast_trace = true, .allow_compaction = true};

  AccelSizes sizes = device->GetBlasSizes(build_desc);
  if (sizes.accel_bytes == 0) return Fail("GetBlasSizes returned 0");
  std::printf("compaction_test: original blas = %llu bytes, scratch = %llu bytes\n",
              (unsigned long long)sizes.accel_bytes, (unsigned long long)sizes.scratch_bytes);

  AccelStructHandle fat = device->CreateAccelStruct(AccelStructType::kBlas, sizes.accel_bytes);
  if (!fat) return Fail("CreateAccelStruct (fat) failed");

  const u32 align = device->caps().accel_scratch_alignment;
  GpuBuffer scratch = device->CreateBuffer(sizes.scratch_bytes + align, kBufferUsageAccelScratch);
  if (!scratch) return Fail("scratch buffer creation failed");
  const u64 scratch_offset = AlignUp(scratch.address, align) - scratch.address;

  AccelCompactionQueryHandle query = device->CreateCompactionQuery(1);
  if (!query) return Fail("CreateCompactionQuery returned null (backend without compaction)");

  // Build the BLAS and record the compacted-size query on the same frame list,
  // then submit. Results are not valid until the frame's fence signals.
  CommandList* cmd = device->BeginFrame(0);
  if (!cmd) return Fail("BeginFrame returned null");
  cmd->BuildBlas(fat, build_desc, scratch, scratch_offset);
  cmd->QueryCompactedSizes(query, &fat, 1);
  device->SubmitFrame(cmd);  // swapchainless

  // Non-blocking poll: legal to return false while the frame is still in flight.
  rx::u64 compacted = 0;
  const bool ready_before = device->GetCompactedSizes(query, &compacted, 1);
  std::printf("compaction_test: poll before wait -> %s\n", ready_before ? "ready" : "not ready");

  device->WaitIdle();  // the frame's fence has now signalled
  if (!device->GetCompactedSizes(query, &compacted, 1))
    return Fail("GetCompactedSizes false after the fence signalled");
  std::printf("compaction_test: compacted blas = %llu bytes (%.1f%% of original)\n",
              (unsigned long long)compacted, 100.0 * (double)compacted / (double)sizes.accel_bytes);

  if (compacted == 0) return Fail("compacted size is 0");
  if (compacted > sizes.accel_bytes) return Fail("compacted size exceeds original");

  // Tight destination + compacting copy.
  AccelStructHandle lean = device->CreateAccelStruct(AccelStructType::kBlas, compacted);
  if (!lean) return Fail("CreateAccelStruct (lean) failed");
  device->ImmediateSubmit([&](CommandList& c) { c.CopyAccelStruct(lean, fat, /*compact=*/true); });

  // Prove the compacted BLAS is a valid, addressable structure: build a TLAS
  // over one instance referencing it. (No trace shader here; a successful,
  // validation-clean build over the compacted address is the proof.)
  const u64 lean_address = device->accel_address(lean);
  if (lean_address == 0) return Fail("compacted blas has no device address");

  if (device->caps().ray_query) {
    TlasInstance inst{};
    inst.transform[0][0] = inst.transform[1][1] = inst.transform[2][2] = 1.0f;
    inst.mask = 0xFF;
    inst.blas_address = lean_address;
    GpuBuffer instances = device->CreateBuffer(
        sizeof(TlasInstance), kBufferUsageAccelBuildInput | kBufferUsageDeviceAddress, true);
    if (!instances || !instances.mapped) return Fail("instance buffer creation failed");
    std::memcpy(instances.mapped, &inst, sizeof(inst));

    AccelSizes tlas_sizes = device->GetTlasSizes(1);
    AccelStructHandle tlas = device->CreateAccelStruct(AccelStructType::kTlas, tlas_sizes.accel_bytes);
    GpuBuffer tlas_scratch =
        device->CreateBuffer(tlas_sizes.scratch_bytes + align, kBufferUsageAccelScratch);
    if (!tlas || !tlas_scratch) return Fail("tlas resources creation failed");

    device->ImmediateSubmit(
        [&](CommandList& c) { c.BuildTlas(tlas, instances, 1, tlas_scratch); });
    device->WaitIdle();
    std::printf("compaction_test: built tlas over the compacted blas (ray_query)\n");

    device->DestroyAccelStruct(tlas);
    device->DestroyBuffer(tlas_scratch);
    device->DestroyBuffer(instances);
  } else {
    std::printf("compaction_test: no ray_query; proved compacted sizes + copy only\n");
  }

  // Retire the fat BLAS through the frame-safe graveyard; drained at teardown.
  device->DestroyAccelStructDeferred(fat);

  device->DestroyAccelStruct(lean);
  device->DestroyCompactionQuery(query);
  device->DestroyBuffer(scratch);
  device->DestroyBuffer(vbo);

  std::printf("compaction_test: PASS\n");
  return 0;
}
