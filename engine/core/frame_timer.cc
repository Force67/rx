#include "core/frame_timer.h"
#include "core/scalar.h"

namespace rx {

FrameTimer::FrameTimer(f64 fixed_step) : fixed_step_(fixed_step), last_(base::TimeTicks::Now()) {}

int FrameTimer::Tick() {
  const base::TimeTicks now = base::TimeTicks::Now();
  frame_delta_ = fixed_delta_ > 0.0 ? fixed_delta_ : (now - last_).InSecondsF();
  last_ = now;
  ++frame_index_;

  // Clamp so a debugger pause does not produce a spiral of death.
  accumulator_ += rx::Min(frame_delta_, 0.25);
  int steps = 0;
  while (accumulator_ >= fixed_step_) {
    accumulator_ -= fixed_step_;
    ++steps;
  }
  return steps;
}

}  // namespace rx
