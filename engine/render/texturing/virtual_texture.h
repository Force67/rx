#ifndef RX_RENDER_VIRTUAL_TEXTURE_H_
#define RX_RENDER_VIRTUAL_TEXTURE_H_

// Feedback-driven sparse virtual texturing. A single large virtual albedo
// space (256x256 pages of 120 payload texels at mip 0, ~30k texels square,
// 9 mip levels) backed by a 4096^2 physical page atlas and a mip-mapped
// indirection texture. The forward pass samples through the indirection
// (falling back to the finest resident ancestor) and appends page requests
// to a feedback buffer from a rotating subset of pixels; the CPU reads the
// requests back a few frames later, generates missing pages on a worker
// thread and streams them into the atlas with LRU eviction.
//
// Page CONTENT is a pluggable generator (procedural by default: a survey
// pattern that makes residency and mip level visible); the machinery -
// feedback, residency, indirection maintenance, streaming - is the product.

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/device.h"

namespace rx::render {

class VirtualTexture {
 public:
  // Mirrored in mesh.ps (SampleVirtualAlbedo); change both together.
  static constexpr u32 kPageStored = 128;  // atlas slot edge, texels
  static constexpr u32 kBorder = 4;        // filter gutter inside a slot
  static constexpr u32 kPayload = kPageStored - 2 * kBorder;  // 120
  static constexpr u32 kVirtualPages = 256;                   // mip 0, per axis
  static constexpr u32 kMips = 9;                             // 256 -> 1 pages
  static constexpr u32 kAtlasPages = 32;                      // 4096 / 128
  static constexpr u32 kFeedbackCapacity = 16384;             // entries
  static constexpr u32 kMaxUploadsPerFrame = 16;

  bool Initialize(Device& device);
  void Destroy(Device& device);
  bool available() const { return static_cast<bool>(atlas_); }

  // Drains the oldest feedback readback, schedules page generation, and
  // records pending page/indirection uploads + the feedback copy/reset for
  // this frame. Call once per frame before the graph executes.
  void AddToGraph(RenderGraph& graph, u64 frame_index);

  TextureView atlas_view() const { return atlas_.view; }
  TextureView indirection_view() const { return indirection_.view; }
  const GpuBuffer& feedback_buffer() const { return feedback_; }
  u32 resident_pages() const { return resident_count_; }

 private:
  struct PageKey {
    u32 mip = 0;
    u32 x = 0;
    u32 y = 0;
  };
  struct GeneratedPage {
    PageKey key;
    std::vector<u8> pixels;  // kPageStored^2 RGBA8, border included
  };
  struct PageState {
    u16 atlas_slot = 0xffff;  // 0xffff = not resident
    u64 last_used = 0;
    bool pending = false;
  };

  u32 PageIndex(const PageKey& key) const;
  PageState& Page(const PageKey& key);
  void GeneratePage(const PageKey& key, std::vector<u8>* pixels) const;
  void WriteIndirection(const PageKey& key);
  void PropagateIndirection(const PageKey& key);
  u16 AcquireSlot(u64 frame_index);

  Device* device_ = nullptr;
  GpuImage atlas_;        // RGBA8 4096^2
  GpuImage indirection_;  // RGBA8 256^2, kMips levels
  GpuBuffer feedback_;    // [0] counter, then packed requests
  static constexpr u32 kReadbackRing = 3;
  GpuBuffer readback_[kReadbackRing];
  GpuBuffer upload_staging_;      // kMaxUploadsPerFrame pages
  GpuBuffer indirection_staging_;  // full pyramid, re-uploaded when dirty

  // CPU residency state, indexed per mip then page.
  std::vector<PageState> pages_[kMips];
  // CPU indirection mirror (RGBA8 per entry), uploaded when dirty.
  std::vector<u8> indirection_cpu_[kMips];
  std::vector<PageKey> slot_owner_ = std::vector<PageKey>(kAtlasPages * kAtlasPages);
  std::vector<bool> slot_used_ = std::vector<bool>(kAtlasPages * kAtlasPages, false);
  u32 resident_count_ = 0;
  bool indirection_dirty_ = true;

  // Generation worker.
  std::thread worker_;
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<PageKey> requests_;
  std::deque<GeneratedPage> completed_;
  bool worker_quit_ = false;
};

}  // namespace rx::render

#endif  // RX_RENDER_VIRTUAL_TEXTURE_H_
