#ifndef RX_RENDER_GPU_PROFILER_H_
#define RX_RENDER_GPU_PROFILER_H_

#include <string>

#include <base/containers/vector.h>

#include "core/types.h"
#include "render/rhi/types.h"

namespace rx::render {

class CommandList;
class Device;

// Per-pass GPU timing via timestamp queries plus debug labels so the same
// pass boundaries show up in capture tools. One query pool per frame in
// flight: a frame's results are read back the next time that slot is reused,
// which the in-flight fence already guarantees is complete.
//
// Every pass is timed. A frame with more passes than its pool holds still
// reports the count, and each slot grows to that size the next time it comes
// round (a pool can only be replaced where its last submission has retired),
// so the miss lasts at most one cycle through the slots.
class GpuProfiler {
 public:
  struct PassTiming {
    std::string name;
    f32 ms = 0.0f;
  };

  bool Initialize(Device& device, u32 frames_in_flight);
  void Shutdown();

  // Resolves the previous results recorded into this slot, then records a
  // query-pool reset into cmd. Call once right after frame recording begins.
  // Latches the detail flag for the frame.
  void BeginFrame(CommandList& cmd, u32 frame_slot);

  // Per-pass timestamps on/off. Each timestamp pair costs real GPU time
  // (barriers + cache flushes around every pass), so detail is only worth
  // paying while something displays the per-pass numbers. Debug labels are
  // free and stay on either way.
  void SetDetail(bool enabled) { detail_ = enabled; }

  // Bracket a render-graph pass. BeginPass opens a debug label and (with
  // detail on) writes a top-of-pipe timestamp; EndPass writes bottom-of-pipe
  // and closes the label.
  void BeginPass(CommandList& cmd, const char* name);
  void EndPass(CommandList& cmd);

  // Whole-frame fallback bracket while detail is off: two timestamps per
  // frame keep total_ms() (dynamic resolution's input) fed. No-ops when the
  // frame latched detail on (the per-pass sum covers it).
  void BeginFrameTotal(CommandList& cmd);
  void EndFrameTotal(CommandList& cmd);

  bool available() const { return device_ != nullptr; }
  // Last fully resolved frame's per-pass timings.
  const base::Vector<PassTiming>& results() const { return results_; }
  f32 total_ms() const { return total_ms_; }

 private:
  static constexpr u32 kQueriesPerPass = 2;  // top-of-pipe + bottom-of-pipe
  // Pool size a slot starts at; enough for a heavy frame, so the growth path
  // below normally never runs.
  static constexpr u32 kInitialPasses = 64;

  struct FramePool {
    TimestampPoolHandle pool;
    base::Vector<std::string> names;  // one per pass, in record order
    u32 pass_count = 0;
    u32 capacity = 0;  // passes the pool holds
    bool recorded = false;
  };

  Device* device_ = nullptr;
  f32 period_ns_ = 0.0f;
  base::Vector<FramePool> frames_;
  u32 current_ = 0;
  bool detail_ = false;
  bool frame_detail_ = false;  // latched at BeginFrame so brackets stay paired
  // Passes bracketed this frame (whether or not the pool had room) and the
  // most any frame has asked for, which every slot's pool grows to.
  u32 requested_ = 0;
  u32 high_water_ = kInitialPasses;

  base::Vector<PassTiming> results_;
  f32 total_ms_ = 0.0f;
};

}  // namespace rx::render

#endif  // RX_RENDER_GPU_PROFILER_H_
