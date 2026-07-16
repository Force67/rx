#include "render/geometry/instance_store.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

#include "render/rhi/swapchain.h"

namespace {

using namespace rx;
using namespace rx::render;

class TestDevice final : public Device {
 public:
  ~TestDevice() override = default;

  void WaitIdle() override {}
  bool RecreateSurface(Window&) override { return false; }
  void DestroySurface() override {}
  std::unique_ptr<Swapchain> CreateSwapchain(u32, u32, bool, bool) override { return {}; }
  MemoryBudget memory_budget() const override { return {}; }

  GpuBuffer CreateBuffer(rx::u64 size, BufferUsageFlags, bool) override {
    auto* data = new u8[size];
    ++live_buffers_;
    return {.handle = {reinterpret_cast<rx::u64>(data)}, .size = size, .mapped = data};
  }
  GpuBuffer CreateBufferWithData(ByteSpan data, BufferUsageFlags usage) override {
    ++data_uploads_;
    GpuBuffer buffer = CreateBuffer(data.size(), usage, true);
    std::memcpy(buffer.mapped, data.data(), data.size());
    return buffer;
  }
  void DestroyBuffer(GpuBuffer& buffer) override {
    if (buffer) {
      delete[] reinterpret_cast<u8*>(buffer.handle.value);
      --live_buffers_;
    }
    buffer = {};
  }

  GpuImage CreateImage2D(Format, Extent2D, TextureUsageFlags, u32, u32) override { return {}; }
  GpuImage CreateImageCube(Format, u32, TextureUsageFlags, u32) override { return {}; }
  void DestroyImage(GpuImage& image) override { image = {}; }
  TextureView CreateMipView(const GpuImage&, u32) override { return {}; }
  TextureView CreateArrayView(const GpuImage&) override { return {}; }
  void DestroyView(TextureView) override {}
  SamplerHandle GetSampler(const SamplerDesc&) override { return {}; }
  PipelineHandle CreateComputePipeline(const ComputePipelineDesc&) override { return {}; }
  PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc&) override { return {}; }
  void DestroyPipeline(PipelineHandle) override {}
  BindingLayoutHandle CreateBindingLayout(const BindingLayoutDesc&) override { return {}; }
  void DestroyBindingLayout(BindingLayoutHandle) override {}
  BindingSetHandle CreateBindingSet(BindingLayoutHandle, u32) override { return {}; }
  void DestroyBindingSet(BindingSetHandle) override {}
  void UpdateBindingSet(BindingSetHandle, std::span<const BindingItem>) override {}
  AccelSizes GetBlasSizes(const BlasBuildDesc&) override { return {}; }
  AccelSizes GetTlasSizes(u32) override { return {}; }
  AccelStructHandle CreateAccelStruct(AccelStructType, rx::u64) override { return {}; }
  void DestroyAccelStruct(AccelStructHandle) override {}
  rx::u64 accel_address(AccelStructHandle) override { return 0; }
  TimestampPoolHandle CreateTimestampPool(u32) override { return {}; }
  void DestroyTimestampPool(TimestampPoolHandle) override {}
  bool GetTimestamps(TimestampPoolHandle, u32, u32, rx::u64*) override { return false; }
  void ImmediateSubmit(const std::function<void(CommandList&)>&) override {}
  CommandList* BeginFrame(u32) override { return nullptr; }
  PresentResult SubmitFrame(CommandList*, Swapchain&, u32) override {
    return PresentResult::kFailed;
  }

  size_t live_buffers() const { return live_buffers_; }
  size_t data_uploads() const { return data_uploads_; }

 private:
  size_t live_buffers_ = 0;
  size_t data_uploads_ = 0;
};

bool Near(f32 a, f32 b) { return std::abs(a - b) < 1e-5f; }

f32 TranslationX(const GpuBuffer& buffer, size_t index = 0) {
  return static_cast<const Mat4*>(buffer.mapped)[index].m[12];
}

int Fail(const char* message) {
  std::fprintf(stderr, "instance_store_test: FAIL: %s\n", message);
  return 1;
}

}  // namespace

int main() {
  TestDevice device;
  InstanceStore store;
  const f32 mesh_center[3] = {1.0f, 0.0f, 0.0f};

  Mat4 transforms[2] = {Mat4::Identity(),
                        MakeTranslation({10.0f, 0.0f, 0.0f}) * MakeScale(2.0f)};
  InstanceGroupHandle first = store.Create(device, 7, transforms, mesh_center, 2.0f);
  if (!first || store.group_count() != 1 || store.instance_count() != 2)
    return Fail("create counters");
  if (device.live_buffers() != 1 || device.data_uploads() != 1)
    return Fail("create device-local upload");

  const InstanceStore::Group& initial = store.groups()[first.index];
  if (!Near(initial.bounds_center.x, 7.5f) || !Near(initial.bounds_center.y, 0.0f) ||
      !Near(initial.bounds_center.z, 0.0f) || !Near(initial.bounds_radius, 8.5f) ||
      !Near(initial.lod_scale, 2.0f) || !initial.cullable) {
    return Fail("conservative group bounds");
  }

  Mat4 replacement = MakeTranslation({20.0f, 0.0f, 0.0f});
  const std::span<const Mat4> replacement_span{&replacement, 1};
  if (!store.Replace(device, first, replacement_span, mesh_center, 2.0f))
    return Fail("replace");
  if (store.instance_count() != 1 || device.live_buffers() != 1)
    return Fail("replace counters or retirement");
  const InstanceStore::Group& updated = store.groups()[first.index];
  if (!Near(updated.bounds_center.x, 21.0f) || !Near(updated.bounds_radius, 2.0f))
    return Fail("updated bounds");
  if (updated.previous_buffer || updated.has_submitted_state)
    return Fail("pre-submit replacement retained motion history");

  store.OnFrameSubmitted(device);
  if (!store.groups()[first.index].has_submitted_state)
    return Fail("submission did not establish instance history");

  Mat4 moved = MakeTranslation({30.0f, 0.0f, 0.0f});
  if (!store.Replace(device, first, std::span<const Mat4>{&moved, 1}, mesh_center, 2.0f))
    return Fail("submitted replacement");
  const InstanceStore::Group& moving = store.groups()[first.index];
  if (!moving.previous_buffer || device.live_buffers() != 2 ||
      !Near(TranslationX(moving.previous_buffer), 20.0f) ||
      !Near(TranslationX(moving.buffer), 30.0f))
    return Fail("replacement motion streams");

  moved = MakeTranslation({40.0f, 0.0f, 0.0f});
  if (!store.Replace(device, first, std::span<const Mat4>{&moved, 1}, mesh_center, 2.0f))
    return Fail("repeated replacement");
  const InstanceStore::Group& repeated = store.groups()[first.index];
  if (device.live_buffers() != 2 || !Near(TranslationX(repeated.previous_buffer), 20.0f) ||
      !Near(TranslationX(repeated.buffer), 40.0f))
    return Fail("repeated replacement lost submitted baseline");

  store.OnFrameSubmitted(device);
  const InstanceStore::Group& submitted = store.groups()[first.index];
  if (submitted.previous_buffer || !submitted.submitted_transforms.empty() ||
      device.live_buffers() != 1)
    return Fail("submission did not retire motion stream");

  Mat4 grown[2] = {MakeTranslation({50.0f, 0.0f, 0.0f}),
                   MakeTranslation({60.0f, 0.0f, 0.0f})};
  if (!store.Replace(device, first, grown, mesh_center, 2.0f)) return Fail("grow replacement");
  const InstanceStore::Group& growth = store.groups()[first.index];
  if (device.live_buffers() != 2 || !Near(TranslationX(growth.previous_buffer, 0), 40.0f) ||
      !Near(TranslationX(growth.previous_buffer, 1), 60.0f))
    return Fail("growth previous stream");

  grown[0] = MakeTranslation({70.0f, 0.0f, 0.0f});
  grown[1] = MakeTranslation({80.0f, 0.0f, 0.0f});
  if (!store.Replace(device, first, grown, mesh_center, 2.0f))
    return Fail("repeated growth replacement");
  const InstanceStore::Group& repeated_growth = store.groups()[first.index];
  if (device.live_buffers() != 2 ||
      !Near(TranslationX(repeated_growth.previous_buffer, 0), 40.0f) ||
      !Near(TranslationX(repeated_growth.previous_buffer, 1), 80.0f))
    return Fail("repeated growth previous stream");

  store.OnFrameSubmitted(device);

  const u32 revision_before_refresh = store.groups()[first.index].revision;
  const f32 refreshed_center[3] = {3.0f, 0.0f, 0.0f};
  store.RefreshMesh(device, 7, refreshed_center, 5.0f, /*compatible=*/true);
  const InstanceStore::Group& refreshed = store.groups()[first.index];
  if (refreshed.revision != revision_before_refresh + 1 || refreshed.bounds_radius <= 5.0f)
    return Fail("compatible mesh refresh did not invalidate bounds consumers");

  if (!store.Destroy(device, first)) return Fail("destroy");
  if (store.group_count() != 0 || store.instance_count() != 0 || device.live_buffers() != 0)
    return Fail("destroy counters or retirement");
  if (store.groups()[first.index].transforms.capacity() != 0)
    return Fail("destroy retained transform capacity");
  if (store.groups()[first.index].submitted_transforms.capacity() != 0)
    return Fail("destroy retained submitted transform capacity");
  if (store.Replace(device, first, replacement_span, mesh_center, 2.0f))
    return Fail("stale handle replaced a dead group");

  InstanceGroupHandle second = store.Create(device, 9, replacement_span, mesh_center, 2.0f);
  if (!second || second.index != first.index || second.generation == first.generation)
    return Fail("slot reuse generation");
  if (store.Destroy(device, first)) return Fail("stale handle destroyed reused slot");
  if (!store.Destroy(device, second)) return Fail("destroy reused slot");

  store.Shutdown(device);
  if (device.live_buffers() != 0) return Fail("shutdown buffer ownership");
  std::printf("instance_store_test: PASS\n");
  return 0;
}
