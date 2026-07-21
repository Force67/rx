// Upload-batch acceptance test. Through the RHI on a surfaceless device:
// create buffers inside a Begin/FlushUploadBatch scope (nested, to exercise the
// depth count) and prove the deferred copies land — after the explicit flush,
// after an implicit flush (ImmediateSubmit reading a just-created buffer), and
// across the soft staging budget (enough data that the batch auto-submits
// early). Contents are verified by copying each buffer back to a host-visible
// readback buffer; ImmediateSubmit's fence wait proves the async batch
// submissions retired, so the mapped reads are safe.
//
// Skips cleanly (exit 0) when no Vulkan driver is present (null backend).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "render/rhi/command_list.h"
#include "render/rhi/device.h"

using namespace rx::render;

namespace {

int Fail(const char* msg) {
  std::fprintf(stderr, "upload_batch_test: FAIL: %s\n", msg);
  return 1;
}

// Uploads `bytes` of a deterministic per-buffer pattern and returns the device
// buffer (created batched when a batch is open).
GpuBuffer UploadPattern(Device& device, u64 bytes, u8 seed, std::vector<u8>& expect) {
  expect.resize(bytes);
  for (u64 i = 0; i < bytes; ++i) expect[i] = static_cast<u8>(seed + i * 31);
  return device.CreateBufferWithData(rx::ByteSpan(expect.data(), expect.size()),
                                     kBufferUsageTransferSrc);
}

// Copies `buffer` back to the host and compares against `expect`.
bool VerifyContents(Device& device, const GpuBuffer& buffer, const std::vector<u8>& expect) {
  GpuBuffer readback =
      device.CreateBuffer(expect.size(), kBufferUsageTransferDst, /*host_visible=*/true);
  if (!readback || !readback.mapped) return false;
  device.ImmediateSubmit([&](CommandList& cmd) {
    cmd.CopyBuffer(buffer, 0, readback, 0, expect.size());
    cmd.MemoryBarrier(BarrierScope::kTransferWrite, BarrierScope::kHostRead);
  });
  const bool ok = std::memcmp(readback.mapped, expect.data(), expect.size()) == 0;
  GpuBuffer retire = readback;
  device.DestroyBuffer(retire);
  return ok;
}

}  // namespace

int main() {
  DeviceDesc desc;
  desc.backend = Backend::kVulkan;
  desc.enable_validation = std::getenv("RX_VALIDATION") != nullptr;
  std::unique_ptr<Device> device = Device::CreateOffscreen(desc);
  if (!device) return Fail("CreateOffscreen returned null");
  if (device->is_stub()) {
    std::printf("upload_batch_test: no vulkan driver, skipping (null backend)\n");
    return 0;
  }
  std::printf("upload_batch_test: device '%s'\n", device->caps().adapter_name.c_str());

  // --- batched uploads, nested scopes, verified after the outermost flush ---
  device->BeginUploadBatch();
  std::vector<u8> expect_a, expect_b;
  GpuBuffer a = UploadPattern(*device, 64 * 1024, 1, expect_a);
  device->BeginUploadBatch();  // nested: must not submit at the inner flush
  GpuBuffer b = UploadPattern(*device, 3 * 1024 + 7, 2, expect_b);
  device->FlushUploadBatch();
  if (!device->UploadBatchActive()) return Fail("inner flush closed the batch");
  device->FlushUploadBatch();
  if (device->UploadBatchActive()) return Fail("outer flush left the batch open");
  if (!a || !b) return Fail("batched buffer creation failed");
  if (!VerifyContents(*device, a, expect_a)) return Fail("buffer A contents wrong after flush");
  if (!VerifyContents(*device, b, expect_b)) return Fail("buffer B contents wrong after flush");

  // --- implicit flush: ImmediateSubmit must see a still-batched buffer ---
  device->BeginUploadBatch();
  std::vector<u8> expect_c;
  GpuBuffer c = UploadPattern(*device, 16 * 1024, 3, expect_c);
  if (!c) return Fail("batched buffer creation failed (implicit-flush case)");
  // VerifyContents runs an ImmediateSubmit while the batch is still open; the
  // implicit flush must submit the pending copy first or the readback is junk.
  if (!VerifyContents(*device, c, expect_c)) return Fail("implicit flush lost a batched copy");
  device->FlushUploadBatch();

  // --- soft staging budget: enough payload that the batch auto-submits early;
  // every buffer must still verify (early chunks and the final flush alike) ---
  constexpr u64 kChunk = 24ull << 20;  // 3 x 24 MiB crosses the 64 MiB budget
  device->BeginUploadBatch();
  std::vector<u8> expect_big[3];
  GpuBuffer big[3];
  for (int i = 0; i < 3; ++i) {
    big[i] = UploadPattern(*device, kChunk, static_cast<u8>(10 + i), expect_big[i]);
    if (!big[i]) return Fail("large batched buffer creation failed");
  }
  device->FlushUploadBatch();
  for (int i = 0; i < 3; ++i) {
    if (!VerifyContents(*device, big[i], expect_big[i]))
      return Fail("large buffer contents wrong across budget auto-submit");
  }

  for (GpuBuffer* buffer : {&a, &b, &c, &big[0], &big[1], &big[2]})
    device->DestroyBuffer(*buffer);
  device->WaitIdle();
  std::printf("upload_batch_test: PASS\n");
  return 0;
}
