#ifndef RX_CORE_JOB_SYSTEM_H_
#define RX_CORE_JOB_SYSTEM_H_

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <base/containers/deque.h>
#include <base/containers/static_function.h>
#include <base/containers/vector.h>

#include "core/export.h"

namespace rx {

class RX_CORE_EXPORT JobSystem {
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

  base::Vector<std::thread> workers_;
  base::SimpleDeque<JobFn> queue_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable idle_;
  std::atomic<unsigned> in_flight_{0};
  bool stop_ = false;
};

}  // namespace rx

#endif  // RX_CORE_JOB_SYSTEM_H_
