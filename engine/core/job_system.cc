#include "base/memory/move.h"
#include "base/threading/lock_guard.h"
#include "core/job_system.h"

namespace rx {

JobSystem::JobSystem(unsigned thread_count) {
  if (thread_count == 0) {
    unsigned hw = base::GetProcessorCount();
    thread_count = hw > 2 ? hw - 1 : 1;
  }
  workers_.reserve(thread_count);
  for (unsigned i = 0; i < thread_count; ++i) {
    workers_.push_back(
        base::MakeUnique<base::Thread>("rx-job", [this] { WorkerLoop(); }, /*start_now=*/true));
  }
}

JobSystem::~JobSystem() {
  {
    base::LockGuard lock(mutex_);
    stop_ = true;
  }
  wake_.NotifyAll();
  for (auto& worker : workers_) worker->Join();
}

void JobSystem::Submit(JobFn job) {
  {
    base::LockGuard lock(mutex_);
    // SimpleDeque only offers a copying push_back; the closure must be
    // copy-constructible anyway for StaticFunction.
    queue_.push_back(job);
  }
  wake_.NotifyOne();
}

void JobSystem::WaitIdle() {
  base::UniqueLock<base::Mutex> lock(mutex_);
  idle_.Wait(lock, [this] { return queue_.empty() && in_flight_.load() == 0; });
}

void JobSystem::WorkerLoop() {
  for (;;) {
    JobFn job;
    {
      base::UniqueLock<base::Mutex> lock(mutex_);
      wake_.Wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (stop_ && queue_.empty()) return;
      job = base::move(queue_.front());
      queue_.pop_front();
      in_flight_.fetch_add(1);
    }
    job();
    in_flight_.fetch_sub(1);
    idle_.NotifyAll();
  }
}

}  // namespace rx
