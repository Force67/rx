#include "render/texturing/virtual_texture.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>

#include "core/log.h"

namespace rx::render {
namespace {

constexpr u32 kAtlasTexels = VirtualTexture::kAtlasPages * VirtualTexture::kPageStored;
constexpr u64 kPageBytes =
    static_cast<u64>(VirtualTexture::kPageStored) * VirtualTexture::kPageStored * 4;

u32 PagesAt(u32 mip) { return VirtualTexture::kVirtualPages >> mip; }

}  // namespace

u32 VirtualTexture::PageIndex(const PageKey& key) const {
  return key.y * PagesAt(key.mip) + key.x;
}

VirtualTexture::PageState& VirtualTexture::Page(const PageKey& key) {
  return pages_[key.mip][PageIndex(key)];
}

bool VirtualTexture::Initialize(Device& device) {
  device_ = &device;
  atlas_ = device.CreateImage2D(Format::kRGBA8Unorm, {kAtlasTexels, kAtlasTexels},
                                kTextureUsageSampled | kTextureUsageTransferDst);
  indirection_ = device.CreateImage2D(Format::kRGBA8Unorm, {kVirtualPages, kVirtualPages},
                                      kTextureUsageSampled | kTextureUsageTransferDst, kMips);
  feedback_ = device.CreateBuffer((1 + kFeedbackCapacity) * sizeof(u32),
                                  kBufferUsageStorage | kBufferUsageTransferSrc |
                                      kBufferUsageTransferDst,
                                  false);
  for (GpuBuffer& rb : readback_) {
    rb = device.CreateBuffer((1 + kFeedbackCapacity) * sizeof(u32),
                             kBufferUsageTransferDst, true);
  }
  upload_staging_ =
      device.CreateBuffer(kPageBytes * kMaxUploadsPerFrame, kBufferUsageTransferSrc, true);
  u64 pyramid_bytes = 0;
  for (u32 m = 0; m < kMips; ++m) {
    pyramid_bytes += static_cast<u64>(PagesAt(m)) * PagesAt(m) * 4;
  }
  indirection_staging_ = device.CreateBuffer(pyramid_bytes, kBufferUsageTransferSrc, true);
  if (!atlas_ || !indirection_ || !feedback_ || !readback_[0].mapped || !upload_staging_.mapped ||
      !indirection_staging_.mapped) {
    RX_WARN("virtual texture allocation failed");
    Destroy(device);
    return false;
  }

  for (u32 m = 0; m < kMips; ++m) {
    pages_[m].assign(static_cast<size_t>(PagesAt(m)) * PagesAt(m), PageState{});
    indirection_cpu_[m].assign(static_cast<size_t>(PagesAt(m)) * PagesAt(m) * 4, 0);
  }
  for (GpuBuffer& rb : readback_) std::memset(rb.mapped, 0, sizeof(u32));

  device.ImmediateSubmit([this](CommandList& cmd) {
    TextureBarrier init[2] = {
        Transition(atlas_, ResourceState::kUndefined, ResourceState::kShaderReadFragment),
        Transition(indirection_, ResourceState::kUndefined,
                   ResourceState::kShaderReadFragment)};
    cmd.TextureBarriers(init);
    cmd.FillBuffer(feedback_, 0, sizeof(u32), 0);
  });

  worker_ = std::thread([this] {
    for (;;) {
      PageKey key;
      {
        std::unique_lock lock(queue_mutex_);
        queue_cv_.wait(lock, [this] { return worker_quit_ || !requests_.empty(); });
        if (worker_quit_) return;
        key = requests_.front();
        requests_.pop_front();
      }
      GeneratedPage page;
      page.key = key;
      GeneratePage(key, &page.pixels);
      std::lock_guard lock(queue_mutex_);
      completed_.push_back(std::move(page));
    }
  });

  // Seed the coarsest page so every pixel has a fallback from frame one.
  {
    std::lock_guard lock(queue_mutex_);
    requests_.push_back({kMips - 1, 0, 0});
    Page({kMips - 1, 0, 0}).pending = true;
  }
  queue_cv_.notify_one();
  return true;
}

void VirtualTexture::Destroy(Device& device) {
  if (worker_.joinable()) {
    {
      std::lock_guard lock(queue_mutex_);
      worker_quit_ = true;
    }
    queue_cv_.notify_all();
    worker_.join();
  }
  if (atlas_) device.DestroyImage(atlas_);
  atlas_ = {};
  if (indirection_) device.DestroyImage(indirection_);
  indirection_ = {};
  if (feedback_) device.DestroyBuffer(feedback_);
  for (GpuBuffer& rb : readback_) {
    if (rb) device.DestroyBuffer(rb);
  }
  if (upload_staging_) device.DestroyBuffer(upload_staging_);
  if (indirection_staging_) device.DestroyBuffer(indirection_staging_);
}

// The demo "megatexture": a continuous survey pattern over the whole virtual
// space - kilometer-grid lines, a hue that drifts across the space, per-page
// hairlines and a mip tint, so residency, borders and LOD are all visible.
void VirtualTexture::GeneratePage(const PageKey& key, std::vector<u8>* pixels) const {
  pixels->resize(kPageBytes);
  const f32 scale = static_cast<f32>(1u << key.mip);  // virtual texels per texel here
  const f32 virtual_size = static_cast<f32>(kVirtualPages) * kPayload;
  for (u32 y = 0; y < kPageStored; ++y) {
    for (u32 x = 0; x < kPageStored; ++x) {
      // Absolute virtual-texel position of this atlas texel (border texels
      // reach into the neighbors, keeping bilinear filtering seamless).
      f32 vx = ((static_cast<f32>(key.x) * kPayload) + (static_cast<f32>(x) - kBorder) + 0.5f) *
               scale;
      f32 vy = ((static_cast<f32>(key.y) * kPayload) + (static_cast<f32>(y) - kBorder) + 0.5f) *
               scale;
      f32 u = vx / virtual_size, v = vy / virtual_size;

      // Base: two-tone checker drifting through hue across the space.
      f32 checker = (static_cast<i32>(std::floor(vx / 64.0f)) ^
                     static_cast<i32>(std::floor(vy / 64.0f))) & 1
                        ? 0.55f
                        : 0.45f;
      f32 r = checker * (0.6f + 0.4f * u);
      f32 g = checker * (0.6f + 0.4f * v);
      f32 b = checker * (0.6f + 0.4f * (1.0f - u));
      // Coarse grid lines every 1024 virtual texels.
      f32 gx = std::fmod(vx, 1024.0f), gy = std::fmod(vy, 1024.0f);
      if (gx < 3.0f * scale || gy < 3.0f * scale) {
        r = g = b = 0.05f;
      }
      // Mip tint: finer pages pull green, coarser pull red.
      f32 t = static_cast<f32>(key.mip) / (kMips - 1);
      r = r * (0.7f + 0.6f * t);
      g = g * (1.3f - 0.6f * t);

      size_t o = (static_cast<size_t>(y) * kPageStored + x) * 4;
      (*pixels)[o + 0] = static_cast<u8>(std::clamp(r, 0.0f, 1.0f) * 255.0f);
      (*pixels)[o + 1] = static_cast<u8>(std::clamp(g, 0.0f, 1.0f) * 255.0f);
      (*pixels)[o + 2] = static_cast<u8>(std::clamp(b, 0.0f, 1.0f) * 255.0f);
      (*pixels)[o + 3] = 255;
    }
  }
}

// Entry for one indirection cell: the finest resident page covering it.
void VirtualTexture::WriteIndirection(const PageKey& key) {
  u32 mip = key.mip, x = key.x, y = key.y;
  for (u32 m = mip; m < kMips; ++m) {
    PageKey cover{m, x >> (m - mip), y >> (m - mip)};
    const PageState& state = pages_[m][PageIndex(cover)];
    if (state.atlas_slot != 0xffff) {
      u8* e = &indirection_cpu_[mip][static_cast<size_t>(PageIndex(key)) * 4];
      e[0] = static_cast<u8>(state.atlas_slot % kAtlasPages);
      e[1] = static_cast<u8>(state.atlas_slot / kAtlasPages);
      e[2] = static_cast<u8>(m);
      e[3] = 255;
      return;
    }
  }
  std::memset(&indirection_cpu_[mip][static_cast<size_t>(PageIndex(key)) * 4], 0, 4);
}

// Refresh every indirection cell in the page's footprint (its own cell plus
// all finer descendants), after a residency change at `key`.
void VirtualTexture::PropagateIndirection(const PageKey& key) {
  for (u32 m = 0; m <= key.mip; ++m) {
    u32 shift = key.mip - m;
    u32 count = 1u << shift;
    for (u32 dy = 0; dy < count; ++dy) {
      for (u32 dx = 0; dx < count; ++dx) {
        WriteIndirection({m, (key.x << shift) + dx, (key.y << shift) + dy});
      }
    }
  }
  indirection_dirty_ = true;
}

u16 VirtualTexture::AcquireSlot(u64 frame_index) {
  for (u32 i = 0; i < slot_used_.size(); ++i) {
    if (!slot_used_[i]) {
      slot_used_[i] = true;
      return static_cast<u16>(i);
    }
  }
  // LRU eviction; never evict a page touched this frame.
  u64 best_age = 0;
  i32 best = -1;
  for (u32 i = 0; i < slot_used_.size(); ++i) {
    const PageKey& owner = slot_owner_[i];
    const PageState& state = pages_[owner.mip][PageIndex(owner)];
    if (state.last_used >= frame_index) continue;
    u64 age = frame_index - state.last_used;
    if (age > best_age) {
      best_age = age;
      best = static_cast<i32>(i);
    }
  }
  if (best < 0) return 0xffff;
  PageKey evicted = slot_owner_[best];
  pages_[evicted.mip][PageIndex(evicted)].atlas_slot = 0xffff;
  --resident_count_;
  PropagateIndirection(evicted);
  return static_cast<u16>(best);
}

void VirtualTexture::AddToGraph(RenderGraph& graph, u64 frame_index) {
  if (!available()) return;

  // 1) Parse the oldest readback slot (written kReadbackRing-1 frames ago,
  // safely past the fence) and turn misses into generation requests.
  const GpuBuffer& rb = readback_[frame_index % kReadbackRing];
  const u32* data = static_cast<const u32*>(rb.mapped);
  u32 count = std::min(data[0], kFeedbackCapacity);
  std::unordered_set<u32> seen;
  u32 enqueued = 0;
  for (u32 i = 0; i < count; ++i) {
    u32 packed = data[1 + i];
    if (!seen.insert(packed).second) continue;
    PageKey key{packed >> 24, packed & 0xfffu, (packed >> 12) & 0xfffu};
    if (key.mip >= kMips) continue;
    u32 pages = PagesAt(key.mip);
    if (key.x >= pages || key.y >= pages) continue;
    PageState& state = Page(key);
    state.last_used = frame_index;
    if (state.atlas_slot != 0xffff || state.pending) continue;
    state.pending = true;
    {
      std::lock_guard lock(queue_mutex_);
      requests_.push_back(key);
    }
    ++enqueued;
  }
  if (enqueued > 0) queue_cv_.notify_one();

  // 2) Collect finished pages (bounded per frame by the staging budget).
  std::vector<GeneratedPage> ready;
  {
    std::lock_guard lock(queue_mutex_);
    while (!completed_.empty() && ready.size() < kMaxUploadsPerFrame) {
      ready.push_back(std::move(completed_.front()));
      completed_.pop_front();
    }
  }
  struct Upload {
    PageKey key;
    u16 slot;
  };
  std::vector<Upload> uploads;
  for (size_t i = 0; i < ready.size(); ++i) {
    GeneratedPage& page = ready[i];
    PageState& state = Page(page.key);
    state.pending = false;
    u16 slot = AcquireSlot(frame_index);
    if (slot == 0xffff) continue;  // atlas exhausted this frame
    std::memcpy(static_cast<u8*>(upload_staging_.mapped) + uploads.size() * kPageBytes,
                page.pixels.data(), kPageBytes);
    state.atlas_slot = slot;
    state.last_used = frame_index;
    slot_owner_[slot] = page.key;
    ++resident_count_;
    PropagateIndirection(page.key);
    uploads.push_back({page.key, slot});
  }

  // 3) Record this frame's GPU work: page + indirection uploads up front...
  const bool upload_indirection = indirection_dirty_;
  indirection_dirty_ = false;
  if (!uploads.empty() || upload_indirection) {
    if (upload_indirection) {
      u64 offset = 0;
      for (u32 m = 0; m < kMips; ++m) {
        std::memcpy(static_cast<u8*>(indirection_staging_.mapped) + offset,
                    indirection_cpu_[m].data(), indirection_cpu_[m].size());
        offset += indirection_cpu_[m].size();
      }
    }
    graph.AddPass(
        "vt_upload", [](RenderGraph::PassBuilder&) {},
        [this, uploads = std::move(uploads), upload_indirection](PassContext& ctx) {
          if (!uploads.empty()) {
            TextureBarrier to_copy[1] = {Transition(atlas_, ResourceState::kShaderReadFragment,
                                                    ResourceState::kCopyDst)};
            ctx.cmd->TextureBarriers(to_copy);
            base::Vector<BufferTextureCopy> regions;
            for (size_t i = 0; i < uploads.size(); ++i) {
              BufferTextureCopy copy;
              copy.buffer_offset = i * kPageBytes;
              copy.offset[0] = static_cast<i32>((uploads[i].slot % kAtlasPages) * kPageStored);
              copy.offset[1] = static_cast<i32>((uploads[i].slot / kAtlasPages) * kPageStored);
              copy.extent = {kPageStored, kPageStored};
              regions.push_back(copy);
            }
            ctx.cmd->CopyBufferToTexture(upload_staging_, atlas_,
                                         {regions.data(), regions.size()});
            TextureBarrier to_read[1] = {
                Transition(atlas_, ResourceState::kCopyDst, ResourceState::kShaderReadFragment)};
            ctx.cmd->TextureBarriers(to_read);
          }
          if (upload_indirection) {
            TextureBarrier to_copy[1] = {Transition(
                indirection_, ResourceState::kShaderReadFragment, ResourceState::kCopyDst)};
            ctx.cmd->TextureBarriers(to_copy);
            base::Vector<BufferTextureCopy> regions;
            u64 offset = 0;
            for (u32 m = 0; m < kMips; ++m) {
              BufferTextureCopy copy;
              copy.buffer_offset = offset;
              copy.mip = m;
              regions.push_back(copy);
              offset += static_cast<u64>(PagesAt(m)) * PagesAt(m) * 4;
            }
            ctx.cmd->CopyBufferToTexture(indirection_staging_, indirection_,
                                         {regions.data(), regions.size()});
            TextureBarrier to_read[1] = {Transition(indirection_, ResourceState::kCopyDst,
                                                    ResourceState::kShaderReadFragment)};
            ctx.cmd->TextureBarriers(to_read);
          }
        });
  }

  // 4) ...and the feedback copy + reset at the tail of the frame.
  graph.AddPass(
      "vt_feedback", [](RenderGraph::PassBuilder&) {},
      [this, frame_index](PassContext& ctx) {
        ctx.cmd->MemoryBarrier(BarrierScope::kAllCommands, BarrierScope::kTransferRead);
        ctx.cmd->CopyBuffer(feedback_, 0, readback_[frame_index % kReadbackRing], 0,
                            (1 + kFeedbackCapacity) * sizeof(u32));
        ctx.cmd->FillBuffer(feedback_, 0, sizeof(u32), 0);
        ctx.cmd->MemoryBarrier(BarrierScope::kTransferWrite, BarrierScope::kAllCommands);
      });
}

}  // namespace rx::render
