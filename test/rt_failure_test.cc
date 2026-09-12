#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>

#include "render/gi/path_tracer.h"
#include "render/gi/raytracing.h"
#include "render/gi/recon_path_tracer.h"
#include "render/gi/skinned_rt.h"
#include "render/gi/restir_di.h"
#include "render/screenspace/reflection_trace.h"
#include "render/rhi/device.h"
#include "render/rhi/swapchain.h"

using namespace rx;
using namespace rx::render;

namespace {

int g_failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
  std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); ++g_failures; \
} } while (0)

class TestCommandList final : public CommandList {
 public:
  void BindPipeline(PipelineHandle) override {}
  void BindSet(u32, BindingSetHandle) override {}
  void BindTransient(u32, std::span<const BindingItem>) override {}
  std::string pass;
  bool expect_reset = false;
  u32 temporal_dispatches = 0;
  void PushConstants(const void* data, u32 size, u32) override {
    if (pass != "recon_temporal" && pass != "recon_spec_temporal") return;
    CHECK(size >= 108);
    f32 reset;
    std::memcpy(&reset, static_cast<const u8*>(data) + 104, sizeof(reset));
    CHECK((reset != 0.0f) == expect_reset);
    ++temporal_dispatches;
  }
  void Dispatch(u32, u32, u32) override {}
  void BeginRendering(const RenderingInfo&) override {}
  void EndRendering() override {}
  void SetViewport(f32, f32, f32, f32) override {}
  void SetScissor(i32, i32, u32, u32) override {}
  void BindVertexBuffer(u32, const GpuBuffer&, rx::u64) override {}
  void BindIndexBuffer(const GpuBuffer&, rx::u64, IndexType) override {}
  void Draw(u32, u32, u32, u32) override {}
  void DrawIndexed(u32, u32, u32, i32, u32) override {}
  void DrawIndexedIndirect(const GpuBuffer&, rx::u64, u32, u32) override {}
  void DrawMeshTasks(u32, u32, u32) override {}
  void TextureBarriers(std::span<const TextureBarrier> barriers) override {
    for (const auto& b : barriers) CHECK(b.texture);
  }
  void MemoryBarrier(BarrierScope, BarrierScope) override {}
  void CopyBufferToTexture(const GpuBuffer&, const GpuImage&,
                           std::span<const BufferTextureCopy>) override {}
  void CopyTextureToBuffer(const GpuImage&, const GpuBuffer&,
                           const BufferTextureCopy&) override {}
  void CopyBuffer(const GpuBuffer&, rx::u64, const GpuBuffer&, rx::u64, rx::u64) override {}
  void BlitMip(const GpuImage&, u32, Extent2D, u32, Extent2D) override {}
  void ResolveTexture(const GpuImage&, const GpuImage&) override {}
  void ClearColor(const GpuImage&, const f32[4]) override {}
  void ClearDepth(const GpuImage&, f32) override {}
  void FillBuffer(const GpuBuffer&, rx::u64, rx::u64, u32) override {}
  void BuildBlas(AccelStructHandle, const BlasBuildDesc&, const GpuBuffer&, rx::u64,
                 AccelStructHandle) override {}
  u32 instances = 0;
  void BuildTlas(AccelStructHandle tlas, const GpuBuffer& input, u32 count,
                  const GpuBuffer& scratch) override {
    CHECK(tlas && input && scratch);
    instances = count;
  }
  void ResetTimestamps(TimestampPoolHandle, u32, u32) override {}
  void WriteTimestamp(TimestampPoolHandle, u32, bool) override {}
  void BeginDebugLabel(const char*) override {}
  void EndDebugLabel() override {}
  void* native_handle() override { return nullptr; }
};

class TestDevice final : public Device {
 public:
  explicit TestDevice(bool async = false) {
    caps_.raytracing = true;
    caps_.accel_scratch_alignment = 256;
    caps_.async_compute = async;
  }
  u32 fail_at = 0;
  u32 attempts = 0;
  rx::u64 next_id = 1;
  std::unordered_set<rx::u64> live;
  rx::u64 Allocate() {
    if (++attempts == fail_at) return 0;
    const rx::u64 id = next_id++;
    live.insert(id);
    return id;
  }
  void Free(rx::u64 id) { if (id) CHECK(live.erase(id) == 1); }

  void WaitIdle() override {}
  bool RecreateSurface(Window&) override { return false; }
  void DestroySurface() override {}
  std::unique_ptr<Swapchain> CreateSwapchain(u32, u32, bool, bool) override { return nullptr; }
  MemoryBudget memory_budget() const override { return {}; }

  GpuBuffer CreateBuffer(rx::u64 size, BufferUsageFlags, bool host) override {
    const rx::u64 id = Allocate();
    if (!id) return {};
    return {.handle = {id}, .size = size, .mapped = host ? std::calloc(1, size) : nullptr,
            .address = id * 4096};
  }
  GpuBuffer CreateBufferWithData(ByteSpan, BufferUsageFlags) override { return {}; }
  void DestroyBuffer(GpuBuffer& buffer) override {
    Free(buffer.handle.value);
    std::free(buffer.mapped);
    buffer = {};
  }
  GpuImage CreateImage2D(Format format, Extent2D extent, TextureUsageFlags, u32, u32) override {
    CHECK(extent.width && extent.height);
    const rx::u64 id = Allocate();
    return {.handle = {id}, .view = {id}, .format = format, .extent = extent};
  }
  GpuImage CreateImageCube(Format, u32, TextureUsageFlags, u32) override { return {}; }
  void DestroyImage(GpuImage& image) override { Free(image.handle.value); image = {}; }
  TextureView CreateMipView(const GpuImage&, u32) override { return {}; }
  TextureView CreateArrayView(const GpuImage&) override { return {}; }
  void DestroyView(TextureView) override {}
  SamplerHandle GetSampler(const SamplerDesc&) override { return {}; }

  PipelineHandle CreateComputePipeline(const ComputePipelineDesc&) override { return {Allocate()}; }
  PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc&) override { return {}; }
  void DestroyPipeline(PipelineHandle pipeline) override { Free(pipeline.value); }
  BindingLayoutHandle CreateBindingLayout(const BindingLayoutDesc&) override { return {}; }
  void DestroyBindingLayout(BindingLayoutHandle) override {}
  BindingSetHandle CreateBindingSet(BindingLayoutHandle, u32) override { return {}; }
  void DestroyBindingSet(BindingSetHandle) override {}
  void UpdateBindingSet(BindingSetHandle, std::span<const BindingItem>) override {}

  AccelSizes GetBlasSizes(const BlasBuildDesc&) override { return {1024, 1024, 512}; }
  AccelSizes GetTlasSizes(u32) override { return {1024, 1024}; }
  AccelStructHandle CreateAccelStruct(AccelStructType, rx::u64) override { return {Allocate()}; }
  void DestroyAccelStruct(AccelStructHandle accel) override { Free(accel.value); }
  rx::u64 accel_address(AccelStructHandle accel) override { return accel.value * 4096; }

  TimestampPoolHandle CreateTimestampPool(u32) override { return {}; }
  void DestroyTimestampPool(TimestampPoolHandle) override {}
  bool GetTimestamps(TimestampPoolHandle, u32, u32, rx::u64*) override { return false; }

  void ImmediateSubmit(const std::function<void(CommandList&)>& record) override {
    TestCommandList cmd;
    record(cmd);
  }
  CommandList* BeginFrame(u32) override { return &cmd_; }
  u32 splits = 0;
  CommandList* SplitFrame(CommandList* cmd, bool) override { ++splits; return cmd; }
  PresentResult SubmitFrame(CommandList*, Swapchain&, u32) override {
    return PresentResult::kFailed;
  }

 private:
  TestCommandList cmd_;
};


}  // namespace

int main() {
  {
    TestDevice device;
    device.fail_at = 1;
    TransientPool pool(device);
    RenderGraph graph;
    graph.CreateTexture({.name = "unused_denoiser_scratch", .width = 17, .height = 9});
    CHECK(graph.Compile(device, pool));
    CHECK(device.attempts == 0);
  }
  {
    TestDevice device(true);
    RenderGraph graph;
    TestCommandList cmd;
    u32 builds = 0;
    graph.AddPass("tlas_build", [](RenderGraph::PassBuilder& b) { b.Async(); },
                   [&](PassContext&) { ++builds; });
    graph.AddPass("query", [](RenderGraph::PassBuilder& b) { b.JoinAsync(); },
                   [&](PassContext&) { CHECK(builds == 1); });
    PassContext ctx{.cmd = &cmd, .device = &device, .graph = &graph};
    graph.Execute(ctx);
    CHECK(builds == 1);
    CHECK(device.splits == 2);
  }
  {
    TestDevice device(true);
    RenderGraph graph;
    TestCommandList cmd;
    u32 builds = 0;
    graph.AddPass("tlas_build", [](RenderGraph::PassBuilder& b) { b.Async(); },
                   [&](PassContext&) { ++builds; });
    graph.AddPass("post", [](RenderGraph::PassBuilder&) {},
                   [&](PassContext&) { CHECK(builds == 1); });
    PassContext ctx{.cmd = &cmd, .device = &device, .graph = &graph};
    graph.Execute(ctx);
    CHECK(builds == 1);
    CHECK(device.splits == 0);
  }
  // Fail each pipeline, camera buffer and history allocation in turn.
  for (u32 fail = 1; fail <= 45; ++fail) {
    TestDevice device;
    device.fail_at = fail;
    ReconPathTracer tracer;
    bool initialized = tracer.Initialize(device, BindingLayoutHandle{1});
    if (!initialized) CHECK(device.live.empty());
    tracer.Resize(device, {17, 9});
    if (initialized && device.attempts >= fail) CHECK(!tracer.available());
    if (initialized) {
      device.fail_at = 0;
      tracer.Resize(device, {17, 9});
      CHECK(tracer.available());
      tracer.Resize(device, {0, 0});
      CHECK(!tracer.available());
      tracer.Resize(device, {19, 11});
      CHECK(tracer.available());
    }
    tracer.Destroy(device);
    tracer.Destroy(device);
    CHECK(device.live.empty());
  }
  for (u32 fail = 1; fail <= 7; ++fail) {
    TestDevice device;
    device.fail_at = fail;
    PathTracer tracer;
    bool initialized = tracer.Initialize(device, BindingLayoutHandle{1});
    if (!initialized) CHECK(device.live.empty());
    tracer.Resize(device, {17, 9});
    if (initialized && device.attempts >= fail) CHECK(!tracer.available());
    if (initialized) {
      device.fail_at = 0;
      tracer.Resize(device, {17, 9});
      CHECK(tracer.available());
      tracer.Resize(device, {0, 0});
      CHECK(!tracer.available());
    }
    tracer.Destroy(device);
    tracer.Destroy(device);
    CHECK(device.live.empty());
  }
  for (u32 fail = 1; fail <= 6; ++fail) {
    TestDevice device;
    device.fail_at = fail;
    RestirDi restir;
    bool initialized = restir.Initialize(device);
    if (!initialized) CHECK(device.live.empty());
    restir.Resize(device, {17, 9});
    CHECK(!restir.available());
    if (initialized) {
      device.fail_at = 0;
      CHECK(restir.Resize(device, {17, 9}));
      CHECK(restir.available());
    }
    restir.Destroy(device);
    CHECK(device.live.empty());
  }
  for (u32 fail = 1; fail <= 4; ++fail) {
    TestDevice device;
    device.fail_at = fail;
    ReflectionTrace reflection;
    CHECK(!reflection.Initialize(device, BindingLayoutHandle{1}));
    CHECK(!reflection.available());
    CHECK(device.live.empty());
    reflection.Destroy(device);
  }
  for (u32 fail = 1; fail <= 3; ++fail) {
    TestDevice device;
    device.fail_at = fail;
    CHECK(!RayTracingContext::Create(device));
    CHECK(device.live.empty());
  }
  {
    TestDevice device;
    auto rt = RayTracingContext::Create(device);
    CHECK(rt);
    TestCommandList cmd;
    base::Vector<RayTracingContext::Instance> instances;
    CHECK(rt->ReserveTlas(0, 1));
    rt->BuildTlas(cmd, 0, 0, instances);
    CHECK(rt->TlasValid(0));
    auto built = rt->tlas(0);
    CHECK(rt->ReserveTlas(0, 65));
    CHECK(!rt->TlasValid(0));
    CHECK(rt->tlas(0) != built);
    rt->BuildTlas(cmd, 0, 1, instances);
    CHECK(rt->TlasValid(0));
    device.fail_at = device.attempts + 1;
    CHECK(!rt->ReserveTlas(0, 129));
    CHECK(!rt->TlasValid(0));
    device.fail_at = 0;
    CHECK(!rt->ReserveTlas(0, ~0u));
    CHECK(!rt->ReserveTlas(~0u, 1));
    CHECK(!rt->TlasValid(~0u));
    CHECK(rt->tlas(~0u));
    rt->BuildTlas(cmd, ~0u, 2, instances);

    base::Vector<AccelTriangles> triangles;
    triangles.push_back({.vertex_address = 4096, .vertex_stride = 12, .vertex_count = 3});
    CHECK(rt->ReserveSkinnedBlas(42, triangles));
    instances.push_back({.mesh_key = 42, .skinned = true, .transform = Mat4::Identity()});
    rt->BuildTlas(cmd, 0, 2, instances);
    CHECK(cmd.instances == 0);
    rt->RecordSkinnedBlas(cmd, 42, 0);
    rt->BuildTlas(cmd, 0, 3, instances);
    CHECK(cmd.instances == 1);
    rt->RemoveSkinnedBlasDeferred(42);
    CHECK(!rt->TlasValid(0));
    rt.reset();
    CHECK(device.live.empty());
  }
  {
    TestDevice device;
    SkinnedRayTracing skin;
    CHECK(skin.Initialize(device));
    const u32 actor = skin.Acquire();
    base::Vector<u32> retired;
    skin.Release(device, nullptr, actor, retired);
    skin.Release(device, nullptr, actor, retired);
    const u32 first = skin.Acquire();
    const u32 second = skin.Acquire();
    CHECK(first == actor);
    CHECK(first != second);
    skin.Destroy(device);
    CHECK(device.live.empty());
  }
  {
    TestDevice device;
    auto rt = RayTracingContext::Create(device);
    ReconPathTracer tracer;
    CHECK(tracer.Initialize(device, BindingLayoutHandle{1}));
    tracer.Resize(device, {17, 9});
    ReconPathTracer::Frame frame;
    frame.frame_index = 100;
    auto run = [&](bool reset, bool external = false) {
      RenderGraph graph;
      TransientPool pool(device);
      TestCommandList cmd;
      cmd.expect_reset = reset;
      graph.SetPassHooks([&](CommandList&, const char* name) { cmd.pass = name; }, {});
      auto output = graph.CreateTexture({.name = "output", .width = 17, .height = 9});
      ReconPathTracer::ExternalInputs guides;
      tracer.AddToGraph(graph, *rt, 0, BindingSetHandle{1}, TextureView{1}, SamplerHandle{1},
                        output, frame, external ? &guides : nullptr);
      CHECK(graph.Compile(device, pool));
      PassContext ctx{.cmd = &cmd, .device = &device, .graph = &graph};
      graph.Execute(ctx);
      CHECK(cmd.temporal_dispatches == (external ? 0u : 2u));
      ++frame.frame_index;
    };
    run(true);
    run(false);
    frame.restir = false;
    run(true);
    frame.restir = true;
    run(true);
    run(false);
    frame.restir_di = false;
    run(true);
    frame.restir_di = true;
    run(true);
    frame.fog = true;
    run(true);
    run(false);
    frame.fog = false;
    run(true);
    run(true, true);
    run(true);
    frame.frame_index += 3;
    run(true);
    tracer.Resize(device, {19, 11});
    run(true);
    tracer.Destroy(device);
    rt.reset();
    CHECK(device.live.empty());
  }
  std::printf("rt_failure_test: %d failures\n", g_failures);
  return g_failures ? 1 : 0;
}
