#include "rxe/render/core/renderer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <base/check.h>
#include <base/option.h>

#include <stb_image_write.h>

#include "base/containers/span.h"
#include "base/containers/vector.h"
#include "base/functional/function.h"
#include "base/memory/mem_ops.h"
#include "base/memory/move.h"
#include "base/memory/unique_pointer.h"
#include "base/optional.h"
#include "base/strings/xstring.h"
#include "base/time/time.h"
#include "foundation/algorithm/sort.h"
#include "foundation/logging/log.h"
#include "foundation/math/scalar.h"
#include "foundation/memory/memory_tracker.h"
#include "rxe/asset/primitives.h"
#include "rxe/asset/texture_compress.h"
#include "rxe/asset/exr_write.h"

namespace rx::render {

void Renderer::CaptureScreenshot(const base::String &path) {
  screenshot_path_ = path;
  screenshot_at_ = -1;
}

void Renderer::WriteHdr() {
  device_->WaitIdle(); // the capture compute wrote hdr_readback_; drain before
                       // reading
  const f32 *src = static_cast<const f32 *>(hdr_readback_.mapped);
  if (!src) {
    hdr_path_.clear();
    return;
  }
  base::Vector<f32> rgb(static_cast<size_t>(hdr_width_) * hdr_height_ * 3);
  for (size_t i = 0; i < static_cast<size_t>(hdr_width_) * hdr_height_; ++i) {
    rgb[i * 3 + 0] = src[i * 4 + 0];
    rgb[i * 3 + 1] = src[i * 4 + 1];
    rgb[i * 3 + 2] = src[i * 4 + 2];
  }
  // .exr (OpenEXR float) is the production container; .hdr (radiance rgbe) is
  // the default. Both store the same linear pre-tonemap scene.
  bool is_exr = hdr_path_.size() >= 4 &&
                hdr_path_.compare(hdr_path_.size() - 4, 4, ".exr") == 0;
  bool ok =
      is_exr
          ? asset::WriteExrRgbF32(hdr_path_, hdr_width_, hdr_height_, rgb.data())
          : stbi_write_hdr(hdr_path_.c_str(), static_cast<int>(hdr_width_),
                           static_cast<int>(hdr_height_), 3, rgb.data()) != 0;
  if (ok) {
    RX_INFO("{} frame written: {} ({}x{})", is_exr ? "exr" : "hdr", hdr_path_,
            hdr_width_, hdr_height_);
  } else {
    RX_WARN("hdr write failed: {}", hdr_path_);
  }
  hdr_path_.clear();
}

// Debug readback for frame generation verification (RX_FRAMEGEN_DUMP): the
// interpolated image should land between the two real frames around it.
void Renderer::DumpFgImage(const gpu::GpuImage &image, gpu::ResourceState state,
                           bool bgra, const char *path) {
  device_->WaitIdle();
  u64 size = static_cast<u64>(image.extent.width) * image.extent.height * 4;
  gpu::GpuBuffer staging =
      device_->CreateBuffer(size, gpu::kBufferUsageTransferDst, true);
  if (!staging.mapped)
    return;
  device_->ImmediateSubmit([&](gpu::CommandList &cmd) {
    cmd.Barrier(gpu::Transition(image, state, gpu::ResourceState::kCopySrc));
    cmd.CopyTextureToBuffer(image, staging, {});
    cmd.Barrier(gpu::Transition(image, gpu::ResourceState::kCopySrc, state));
  });
  base::Vector<u8> pixels(static_cast<size_t>(image.extent.width) *
                          image.extent.height * 3);
  const u8 *src = static_cast<const u8 *>(staging.mapped);
  for (size_t i = 0;
       i < static_cast<size_t>(image.extent.width) * image.extent.height; ++i) {
    pixels[i * 3 + 0] = src[i * 4 + (bgra ? 2 : 0)];
    pixels[i * 3 + 1] = src[i * 4 + 1];
    pixels[i * 3 + 2] = src[i * 4 + (bgra ? 0 : 2)];
  }
  device_->DestroyBuffer(staging);
  if (stbi_write_png(path, static_cast<int>(image.extent.width),
                     static_cast<int>(image.extent.height), 3, pixels.data(),
                     static_cast<int>(image.extent.width) * 3)) {
    RX_INFO("framegen dump written: {}", path);
  }
}

void Renderer::WriteBackbufferPng(const base::String &path) {
  device_->WaitIdle();
  gpu::Extent2D extent = swapchain_->extent();
  u64 size = static_cast<u64>(extent.width) * extent.height * 4;
  gpu::GpuBuffer staging =
      device_->CreateBuffer(size, gpu::kBufferUsageTransferDst, true);
  if (!staging.mapped)
    return;

  device_->ImmediateSubmit([&](gpu::CommandList &cmd) {
    cmd.CopyTextureToBuffer(capture_image_, staging, {});
    cmd.MemoryBarrier(gpu::BarrierScope::kTransferWrite, gpu::BarrierScope::kHostRead);
  });

  // Swapchain is bgra; png wants rgb.
  base::Vector<u8> pixels(static_cast<size_t>(extent.width) * extent.height *
                          3);
  const u8 *src = static_cast<const u8 *>(staging.mapped);
  for (size_t i = 0; i < static_cast<size_t>(extent.width) * extent.height;
       ++i) {
    pixels[i * 3 + 0] = src[i * 4 + 2];
    pixels[i * 3 + 1] = src[i * 4 + 1];
    pixels[i * 3 + 2] = src[i * 4 + 0];
  }
  device_->DestroyBuffer(staging);
  if (stbi_write_png(path.c_str(), static_cast<int>(extent.width),
                     static_cast<int>(extent.height), 3, pixels.data(),
                     static_cast<int>(extent.width) * 3)) {
    RX_INFO("screenshot written: {}", path);
    // Alongside the capture because that is where a before/after comparison is
    // made: a headless run has no debug overlay, so without this the only
    // report of resident texture memory is a tab nobody can open.
    LogTextureMemory();
  } else {
    RX_WARN("screenshot write failed: {}", path);
  }
}

void Renderer::WriteScreenshot() {
  WriteBackbufferPng(screenshot_path_);
  screenshot_path_.clear();
}

bool Renderer::CaptureArmed() const {
  return !screenshot_path_.empty() ||
         (!seq_prefix_.empty() && seq_written_ < seq_count_);
}

bool Renderer::EnsureCaptureImage() {
  gpu::Extent2D extent = swapchain_->extent();
  if (capture_image_.handle && capture_image_.extent.width == extent.width &&
      capture_image_.extent.height == extent.height)
    return true;
  // Deferred: the frame that last rendered into it completes without a fence
  // wait, so it can still be in flight when a resize lands mid capture run.
  if (capture_image_.handle)
    device_->DestroyImageDeferred(capture_image_);
  // Same format as the backbuffer so every pass that writes it (post, UI)
  // behaves identically; TransferSrc is what the png readback copies from.
  capture_image_ = device_->CreateImage2D(
      swapchain_->format(), extent,
      gpu::kTextureUsageColorTarget | gpu::kTextureUsageTransferSrc |
          gpu::kTextureUsageTransferDst | gpu::kTextureUsageSampled);
  if (!capture_image_.handle) {
    RX_WARN("offscreen capture image allocation failed");
    return false;
  }
  return true;
}

} // namespace rx::render
