#ifndef RX_FOUNDATION_TASKS_JOB_SYSTEM_H_
#define RX_FOUNDATION_TASKS_JOB_SYSTEM_H_

#include <base/containers/deque.h>
#include <base/containers/static_function.h>
#include <base/containers/vector.h>

#include "base/atomic.h"
#include "base/memory/unique_pointer.h"
#include "base/threading/condition_variable.h"
#include "base/threading/mutex.h"
#include "base/threading/thread.h"
#include "foundation/build_config/export.h"

namespace rx {

class RX_FOUNDATION_EXPORT JobSystem {
 public:
  // Closures are stored inline; captures must fit and be copy-constructible.
  using JobFn = base::StaticFunction<void(), 256>;

  explicit JobSystem(unsigned thread_count = 0);
  ~JobSystem();

  JobSystem(const JobSystem&) = delete;
  JobSystem& operator=(const JobSystem&) = delete;

  void Submit(JobFn job);
  void WaitIdle();

  unsigned thread_count() const { return static_cast<unsigned>(workers_.size()); }

 private:
  void WorkerLoop();

  // base::Thread is not movable; the vector must not relocate live threads.
  base::Vector<base::UniquePointer<base::Thread>> workers_;
  base::SimpleDeque<JobFn> queue_;
  base::Mutex mutex_;
  base::ConditionVariable wake_;
  base::ConditionVariable idle_;
  base::Atomic<unsigned> in_flight_{0};
  bool stop_ = false;
};

}  // namespace rx

#endif  // RX_FOUNDATION_TASKS_JOB_SYSTEM_H_
