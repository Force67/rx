#include "render/util/gpu_profiler.h"

#include "render/rhi/command_list.h"
#include "render/rhi/device.h"

namespace rx::render {

bool GpuProfiler::Initialize(Device& device, u32 frames_in_flight) {
  if (device.is_stub()) return false;
  f32 period = device.caps().timestamp_period;
  if (period <= 0.0f) return false;  // no timestamp support, profiler stays off

  period_ns_ = period;
  frames_.clear();
  for (u32 i = 0; i < frames_in_flight; ++i) {
    FramePool fp;
    fp.pool = device.CreateTimestampPool(kInitialPasses * kQueriesPerPass);
    if (!fp.pool) return false;
    fp.capacity = kInitialPasses;
    frames_.push_back(std::move(fp));
  }
  device_ = &device;
  return true;
}

void GpuProfiler::Shutdown() {
  if (!device_) return;
  for (FramePool& fp : frames_) {
    if (fp.pool) device_->DestroyTimestampPool(fp.pool);
  }
  frames_.clear();
  device_ = nullptr;
}

void GpuProfiler::BeginFrame(CommandList& cmd, u32 frame_slot) {
  if (!device_ || frames_.empty()) return;
  current_ = frame_slot % static_cast<u32>(frames_.size());
  FramePool& fp = frames_[current_];

  // The fence for this slot already fired, so the timestamps from its previous
  // use are readable. Convert pairs to milliseconds.
  if (fp.recorded && fp.pass_count > 0) {
    base::Vector<u64> stamps(fp.pass_count * kQueriesPerPass);
    if (device_->GetTimestamps(fp.pool, 0, fp.pass_count * kQueriesPerPass, stamps.data())) {
      results_.clear();
      total_ms_ = 0.0f;
      for (u32 i = 0; i < fp.pass_count; ++i) {
        u64 begin = stamps[i * kQueriesPerPass];
        u64 end = stamps[i * kQueriesPerPass + 1];
        f32 ms = end > begin ? static_cast<f32>((end - begin) * period_ns_) * 1e-6f : 0.0f;
        results_.push_back({fp.names[i], ms});
        total_ms_ += ms;
      }
    }
  }

  // Everything this slot submitted has retired (that is what let us read the
  // timestamps above), so its pool is free to be replaced by one big enough for
  // the heaviest frame seen since.
  if (fp.capacity < high_water_) {
    if (TimestampPoolHandle grown =
            device_->CreateTimestampPool(high_water_ * kQueriesPerPass)) {
      device_->DestroyTimestampPool(fp.pool);
      fp.pool = grown;
      fp.capacity = high_water_;
    }
  }

  cmd.ResetTimestamps(fp.pool, 0, fp.capacity * kQueriesPerPass);
  fp.names.clear();
  fp.pass_count = 0;
  requested_ = 0;
  fp.recorded = true;
  frame_detail_ = detail_;
}

void GpuProfiler::BeginPass(CommandList& cmd, const char* name) {
  if (!device_ || frames_.empty()) return;
  FramePool& fp = frames_[current_];
  cmd.BeginDebugLabel(name);  // labels are free; captures stay readable
  if (!frame_detail_) return;
  // Counted even when the pool is full, which is what makes the slot grow.
  ++requested_;
  if (requested_ > high_water_) high_water_ = requested_;
  if (fp.pass_count >= fp.capacity) return;
  fp.names.push_back(name);
  cmd.WriteTimestamp(fp.pool, fp.pass_count * kQueriesPerPass, /*after_work=*/false);
}

void GpuProfiler::EndPass(CommandList& cmd) {
  if (!device_ || frames_.empty()) return;
  FramePool& fp = frames_[current_];
  if (frame_detail_ && fp.pass_count < fp.capacity) {
    cmd.WriteTimestamp(fp.pool, fp.pass_count * kQueriesPerPass + 1, /*after_work=*/true);
    ++fp.pass_count;
  }
  cmd.EndDebugLabel();
}

void GpuProfiler::BeginFrameTotal(CommandList& cmd) {
  if (!device_ || frames_.empty() || frame_detail_) return;
  FramePool& fp = frames_[current_];
  fp.names.push_back("frame");
  cmd.WriteTimestamp(fp.pool, fp.pass_count * kQueriesPerPass, /*after_work=*/false);
}

void GpuProfiler::EndFrameTotal(CommandList& cmd) {
  if (!device_ || frames_.empty() || frame_detail_) return;
  FramePool& fp = frames_[current_];
  if (fp.pass_count < fp.capacity) {
    cmd.WriteTimestamp(fp.pool, fp.pass_count * kQueriesPerPass + 1, /*after_work=*/true);
    ++fp.pass_count;
  }
}

}  // namespace rx::render
