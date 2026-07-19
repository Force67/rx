#ifndef RX_RENDER_RHI_DEVICE_H_
#define RX_RENDER_RHI_DEVICE_H_

#include <functional>
#include <memory>
#include <string>

#include <base/containers/vector.h>

#include "core/types.h"
#include "core/window.h"
#include "render/rhi/bindings.h"
#include "render/rhi/command_list.h"
#include "render/rhi/pipeline.h"
#include "render/rhi/resources.h"
#include "render/rhi/types.h"

namespace rx::render {

class Swapchain;

enum class Backend : u8 {
  kAuto = 0,  // first available: platform native, then vulkan, then null
  kVulkan,
  kD3D12,
  kNull,  // headless stub: creates nothing, records nothing; keeps the
          // renderer's control flow testable without a GPU or a loader
};

const char* BackendName(Backend backend);

struct DeviceDesc {
  Backend backend = Backend::kAuto;
  bool enable_validation = false;
  bool request_raytracing = true;
  // Additional device extensions an app's own GPU passes need (e.g. a game with
  // a custom Vulkan pipeline recorded through the scene hooks). Each is enabled
  // only if the adapter advertises it; the granted set lands in
  // DeviceCaps::extra_extensions. Vulkan-only; ignored by other backends. The
  // baseline rx enables (BDA, descriptor indexing, timeline, dynamic rendering,
  // sync2, and every core / Vulkan 1.1-1.3 feature bit the driver reports, plus
  // mesh-shader and ray-query when present) does not need listing here.
  base::Vector<std::string> extra_device_extensions;
};

// What the picked GPU actually supports. Optional features are queried,
// never assumed, so the same binary runs on a desktop GPU and an android
// phone. Per-backend baselines: Vulkan 1.3 with dynamic rendering,
// synchronization2, buffer device address, descriptor indexing and timeline
// semaphores; D3D12 feature level 12_1 with SM 6.6 and enhanced barriers.
struct DeviceCaps {
  Backend backend = Backend::kNull;
  std::string adapter_name;
  u32 api_version = 0;
  bool raytracing = false;  // acceleration structures + ray tracing pipeline
  bool ray_query = false;
  bool mesh_shaders = false;
  bool fragment_shading_rate = false;  // attachment-based VRS (rate image)
  u32 shading_rate_texel = 16;         // rate image texel size in pixels
  u32 shading_rate_max_size = 1;       // largest fragment edge (4 = 4x4 allowed)
  bool fill_mode_non_solid = false;  // wireframe debug views
  f32 max_anisotropy = 1.0f;         // 1 = anisotropic filtering unavailable
  f32 timestamp_period = 0.0f;       // ns per timestamp tick, 0 = no gpu timing
  bool debug_utils = false;          // debug markers/labels available
  bool integrated = false;           // integrated/handheld gpu (shared memory)
  u64 device_local_bytes = 0;        // summed device-local heap size, vram proxy
  u32 accel_scratch_alignment = 256;  // min scratch offset alignment for AS builds
  bool async_compute = false;  // second same-family queue for overlapped compute
  // 64-bit min/max/exchange atomics on storage buffers (vulkan
  // shaderBufferInt64Atomics). The virtual-geometry software rasterizer packs
  // depth|payload into a u64 and resolves visibility with one InterlockedMax.
  bool buffer_atomics64 = false;
  // App-requested device extensions (DeviceDesc::extra_device_extensions) that
  // the adapter actually granted. An app checks this to know whether its custom
  // GPU pass can use a given extension.
  base::Vector<std::string> extra_extensions;
};

enum class AccelStructType : u8 { kBlas, kTlas };

struct AccelSizes {
  u64 accel_bytes = 0;
  u64 scratch_bytes = 0;
};

enum class PresentResult : u8 {
  kOk,
  kOutOfDate,  // recreate the swapchain (includes real-resize suboptimal)
  kFailed,
};

// Owns the GPU: adapter selection, resource + pipeline creation, per-frame
// command recording and submission. Create() picks the backend from the desc;
// a null-backend device (is_stub() true) comes back when no loader, no
// capable GPU or no presentable window is available, and every operation on
// it is a safe no-op.
class Device {
 public:
  static constexpr u32 kMaxFramesInFlight = 2;

  static std::unique_ptr<Device> Create(const DeviceDesc& desc, Window& window);

  // Surfaceless device: a real GPU device with the same feature enablement and
  // caps as the windowed path, but with no presentation surface or swapchain.
  // The frame ring, fences, immediate submit, resource and pipeline creation
  // all work identically; only Acquire/Present are absent. Drive a frame with
  // BeginFrame -> record -> SubmitFrame(cmd) (the swapchainless overload) and
  // read pixels back with ReadbackImage. Picks the Vulkan backend; falls back
  // to the null backend when no Vulkan driver is present (is_stub() true), so
  // callers get a valid, safe device on any machine. D3D12 offscreen is not
  // wired yet (it also falls back to null).
  static std::unique_ptr<Device> CreateOffscreen(const DeviceDesc& desc);
  virtual ~Device() = default;

  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  const DeviceCaps& caps() const { return caps_; }
  bool is_stub() const { return caps_.backend == Backend::kNull; }

  virtual void WaitIdle() = 0;

  // Rebinds the presentation surface to a (possibly new) window after it was
  // lost, for platforms that destroy and recreate the surface across the app
  // lifecycle (Android background/foreground). Adapter and queues are
  // unchanged. Returns false if the new surface could not be created.
  virtual bool RecreateSurface(Window& window) = 0;

  // Destroys the presentation surface (its swapchain must already be gone).
  virtual void DestroySurface() = 0;

  // `hdr` requests an HDR-capable surface format (HDR10 PQ preferred, scRGB
  // fallback); silently falls back to SDR when the surface has neither.
  virtual std::unique_ptr<Swapchain> CreateSwapchain(u32 width, u32 height, bool vsync,
                                                     bool hdr = false) = 0;

  // Live gpu memory usage, summed over the device-local heaps, for the debug
  // overlay. budget is the driver's estimate of what the app may use.
  struct MemoryBudget {
    u64 used_bytes = 0;
    u64 budget_bytes = 0;
    u64 allocated_bytes = 0;
    u32 allocation_count = 0;
  };
  virtual MemoryBudget memory_budget() const = 0;

  // --- resources ---
  virtual GpuBuffer CreateBuffer(u64 size, BufferUsageFlags usage, bool host_visible = false) = 0;
  virtual GpuBuffer CreateBufferWithData(ByteSpan data, BufferUsageFlags usage) = 0;
  // Immediate destruction: the caller guarantees the GPU is done with the
  // resource (no in-flight frame references it). For a resource that may still
  // be read by a submitted-but-unfinished frame use the *Deferred variants.
  virtual void DestroyBuffer(GpuBuffer& buffer) = 0;

  // samples > 1 creates a multisampled image (mip_levels must stay 1); resolve
  // through CommandList::ResolveTexture before any single-sampled consumer.
  virtual GpuImage CreateImage2D(Format format, Extent2D extent, TextureUsageFlags usage,
                                 u32 mip_levels = 1, u32 samples = 1) = 0;
  // Volumetric texture (froxel scattering volumes). Backends without it
  // return a null image and the caller skips the feature.
  virtual GpuImage CreateImage3D(Format /*format*/, u32 /*width*/, u32 /*height*/, u32 /*depth*/,
                                 TextureUsageFlags /*usage*/) {
    return {};
  }
  virtual GpuImage CreateImageCube(Format format, u32 size, TextureUsageFlags usage,
                                   u32 mip_levels = 1) = 0;
  // Layered 2D image (texture atlas arrays, GPU-driven material stacks). The
  // whole-image default view is a 2D_ARRAY over every layer and mip; upload a
  // layer with CopyBufferToTexture (BufferTextureCopy::array_layer). Backends
  // without it return a null image and the caller skips the feature.
  virtual GpuImage CreateImage2DArray(Format /*format*/, u32 /*width*/, u32 /*height*/,
                                      u32 /*array_layers*/, TextureUsageFlags /*usage*/,
                                      u32 /*mip_levels*/ = 1) {
    return {};
  }
  virtual void DestroyImage(GpuImage& image) = 0;
  // Extra single-mip view for mip-chained images (bloom pyramid).
  virtual TextureView CreateMipView(const GpuImage& image, u32 mip) = 0;
  // 2D-array view over the whole image, for shaders that declare
  // Texture2DArray over a (possibly single-layer) atlas.
  virtual TextureView CreateArrayView(const GpuImage& image) = 0;
  virtual void DestroyView(TextureView view) = 0;

  // Cached; valid for the device's lifetime, never destroyed by callers.
  virtual SamplerHandle GetSampler(const SamplerDesc& desc) = 0;

  // --- pipelines & bindings ---
  virtual PipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc) = 0;
  virtual PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;
  virtual void DestroyPipeline(PipelineHandle pipeline) = 0;

  // Bulk pipeline creation (engine startup): between Begin and End the
  // Create*Pipeline calls may return handles whose driver object is still
  // compiling on worker threads - binding such a handle blocks until it is
  // ready. EndPipelineBatch joins the workers and returns false if any
  // creation failed (individual handles cannot signal failure while a batch
  // is open). Backends without an implementation stay synchronous.
  virtual void BeginPipelineBatch() {}
  virtual bool EndPipelineBatch() { return true; }

  // Explicit layouts, for sets shared across pipelines (bindless registry,
  // frame globals). Pass-local sets skip this and declare slots inline in the
  // pipeline desc.
  virtual BindingLayoutHandle CreateBindingLayout(const BindingLayoutDesc& desc) = 0;
  virtual void DestroyBindingLayout(BindingLayoutHandle layout) = 0;
  // Persistent set (lives across frames, updated incrementally). Transient
  // per-record sets go through CommandList::BindTransient instead.
  virtual BindingSetHandle CreateBindingSet(BindingLayoutHandle layout,
                                            u32 variable_count = 0) = 0;
  virtual void DestroyBindingSet(BindingSetHandle set) = 0;
  virtual void UpdateBindingSet(BindingSetHandle set, std::span<const BindingItem> items) = 0;
  void UpdateBindingSet(BindingSetHandle set, std::initializer_list<BindingItem> items) {
    UpdateBindingSet(set, std::span<const BindingItem>(items.begin(), items.size()));
  }

  // --- acceleration structures (caps().ray_query gated) ---
  virtual AccelSizes GetBlasSizes(const BlasBuildDesc& desc) = 0;
  virtual AccelSizes GetTlasSizes(u32 instance_count) = 0;
  virtual AccelStructHandle CreateAccelStruct(AccelStructType type, u64 size) = 0;
  virtual void DestroyAccelStruct(AccelStructHandle accel) = 0;
  virtual u64 accel_address(AccelStructHandle accel) = 0;

  // Compacted-size query pool: `count` slots, one per acceleration structure a
  // CommandList::QueryCompactedSizes call reports on. Reuse the object across
  // frames (each QueryCompactedSizes re-resets the slots it writes). Returns a
  // null handle on backends without compaction support (caller then skips it).
  virtual AccelCompactionQueryHandle CreateCompactionQuery(u32 /*count*/) { return {}; }
  virtual void DestroyCompactionQuery(AccelCompactionQueryHandle /*query*/) {}
  // Non-blocking read of the compacted sizes (bytes) written by the matching
  // QueryCompactedSizes. Returns false until every slot's result is ready, i.e.
  // until the fence for the submission that carried the QueryCompactedSizes has
  // signalled (poll it the next frame, or call it straight after ImmediateSubmit
  // whose blocking wait guarantees readiness). On true, out[0..count) hold the
  // sizes to pass to CreateAccelStruct before a compacting CopyAccelStruct.
  virtual bool GetCompactedSizes(AccelCompactionQueryHandle /*query*/, u64* /*out*/,
                                 u32 /*count*/) {
    return false;
  }

  // --- frame-safe deferred destruction ---
  // Retire a resource that a submitted-but-not-yet-finished frame may still
  // reference. The resource is parked in a per-frame-slot graveyard and freed
  // only once a fence proves every frame that could have touched it has
  // completed (drained at BeginFrame after the slot's fence wait, and fully at
  // WaitIdle / device teardown). Use this instead of the immediate Destroy* for
  // per-frame churned resources (e.g. a buffer resized mid-frame): the handle is
  // nulled out immediately, but the GPU object outlives the call by up to
  // kMaxFramesInFlight frames. Backends without a frame ring (null) fall back to
  // immediate destruction, which is safe there because nothing is in flight.
  virtual void DestroyBufferDeferred(GpuBuffer& buffer) { DestroyBuffer(buffer); }
  virtual void DestroyImageDeferred(GpuImage& image) { DestroyImage(image); }
  virtual void DestroyAccelStructDeferred(AccelStructHandle accel) { DestroyAccelStruct(accel); }

  // --- profiling ---
  virtual TimestampPoolHandle CreateTimestampPool(u32 count) = 0;
  virtual void DestroyTimestampPool(TimestampPoolHandle pool) = 0;
  // Copies available results (ticks) for [first, first+count); returns false
  // while the range is still in flight.
  virtual bool GetTimestamps(TimestampPoolHandle pool, u32 first, u32 count, u64* out) = 0;

  // --- recording & submission ---
  // Records into a transient command list and blocks until execution
  // finished. For uploads and one-off transitions, not the frame path.
  virtual void ImmediateSubmit(const std::function<void(CommandList&)>& record) = 0;

  // --- coalesced uploads ---
  // Opens/closes a transfer batch: while open, CreateBufferWithData records its
  // staging copy into one shared command buffer and defers its submit, instead
  // of doing a blocking ImmediateSubmit per buffer. FlushUploadBatch submits the
  // accumulated copies once and blocks until they finish; the batch is also
  // flushed implicitly by ImmediateSubmit / BeginFrame / WaitIdle so any GPU
  // work that reads a just-created buffer (e.g. a BLAS build) sees it uploaded.
  // Nestable via a depth count (only the outermost FlushUploadBatch submits).
  // Buffers created inside the batch are valid to use only after the flush.
  // Default no-op: backends without the optimization just keep submitting per
  // buffer, which stays correct. Streaming wraps a frame's uploads in one batch
  // so a burst of new cells no longer pays a blocking round-trip per buffer.
  virtual void BeginUploadBatch() {}
  virtual void FlushUploadBatch() {}

  // True while a batch (see BeginUploadBatch) is open. Callers that stage their
  // own uploads (e.g. texture image copies) check this to decide whether their
  // staging must survive until the batch flush.
  virtual bool UploadBatchActive() const { return false; }
  // Records an upload's copy/barrier commands into the open batch's command
  // buffer, or, with no batch open, runs them through a blocking ImmediateSubmit
  // (the default). Lets a caller share the coalesced submit without knowing the
  // backend. `record` runs synchronously (commands are recorded now); the work
  // completes at the next flush/frame when batched.
  virtual void RecordUpload(const std::function<void(CommandList&)>& record) {
    ImmediateSubmit(record);
  }
  // Hands a staging buffer to the open batch to free once its copies have run
  // (only call while UploadBatchActive()); frees immediately otherwise.
  virtual void ParkBatchStaging(GpuBuffer& buffer) { DestroyBuffer(buffer); }

  // Frame ring: waits for `slot`'s previous submission, resets its command
  // allocator and transient binding pool, begins recording. Slots cycle
  // 0..kMaxFramesInFlight-1.
  virtual CommandList* BeginFrame(u32 slot) = 0;
  // Ends recording, submits waiting on the slot's swapchain acquire, presents
  // image_index. Backends fold "suboptimal but same extent" (Android rotation)
  // into kOk; kOutOfDate means the caller must recreate the swapchain.
  virtual PresentResult SubmitFrame(CommandList* cmd, Swapchain& swapchain, u32 image_index) = 0;

  // Swapchainless submit: ends recording and submits the frame's work,
  // signaling the slot's fence, with no Acquire and no Present. This is the
  // frame-completion half of the offscreen contract (the windowed path uses
  // the swapchain overload above; both share BeginFrame). A windowed device may
  // also use it for a frame that renders but does not present. The async-compute
  // join (SplitFrame/BeginAsync/SubmitAsync) still applies. Default no-op so the
  // null backend stays inert; the Vulkan backend implements it.
  virtual void SubmitFrame(CommandList* /*cmd*/) {}

  // Copies mip 0 of `image` into `out`. `current` is the image's present state
  // (e.g. kColorTarget straight after rendering); the readback transitions it to
  // kCopySrc internally, CopyTextureToBuffer's it into a host-visible staging
  // buffer, host-read-barriers and drives it through ImmediateSubmit, then
  // memcpys `out_size` bytes (which must be at least
  // width*height*FormatTexelBytes(format)). The image must have been created
  // with kTextureUsageTransferSrc. Works on windowed and offscreen devices.
  // Returns false when out_size is too small or the backend has no readback
  // (null/d3d12 report false honestly).
  virtual bool ReadbackImage(const GpuImage& /*image*/, ResourceState /*current*/, void* /*out*/,
                             size_t /*out_size*/) {
    return false;
  }

  // --- async compute (optional; see caps().async_compute) ---
  // A second queue of the same family overlaps flagged compute passes with the
  // graphics timeline (same family = no ownership transfers, semaphore-only
  // sync). SplitFrame ends the current graphics segment, submits it (signaling
  // the fork point when asked) and returns the next segment's list; BeginAsync
  // returns the slot's compute list; SubmitAsync submits it waiting on the
  // fork and signaling completion. The final segment goes through SubmitFrame,
  // which additionally waits on the async submission when one happened this
  // frame. Defaults are inert so backends without the feature stay linear.
  virtual CommandList* SplitFrame(CommandList* cmd, bool /*signal_fork*/) { return cmd; }
  virtual CommandList* BeginAsync() { return nullptr; }
  virtual void SubmitAsync(CommandList* /*cmd*/) {}

  // --- frame generation present (optional; vulkan only today) ---
  // Ends recording and submits waiting on BOTH of the slot's acquires (the
  // regular one and Swapchain::AcquireSecond's), then presents interp_index
  // followed by real_index. Under FIFO the two presents land a vblank apart,
  // which is the whole pacing story.
  virtual PresentResult SubmitFrameGen(CommandList* /*cmd*/, Swapchain& /*swapchain*/,
                                       u32 /*interp_index*/, u32 /*real_index*/) {
    return PresentResult::kFailed;
  }

 protected:
  Device() = default;

  DeviceCaps caps_;
};

}  // namespace rx::render

#endif  // RX_RENDER_RHI_DEVICE_H_
