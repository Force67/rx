#ifndef RX_RENDER_VULKAN_VMA_CONFIG_H_
#define RX_RENDER_VULKAN_VMA_CONFIG_H_

// VMA's configuration hooks, pointed at base and rx instead of the STL pieces
// VMA falls back to (std::mutex, std::shared_mutex, std::atomic, std::sort,
// std::min/max). Included through VMA_CONFIGURATION_USER_INCLUDES_H, which
// also keeps VMA from pulling <algorithm> and <mutex> for its defaults. Only
// the implementation half of vk_mem_alloc.h uses these, so vma_impl.cc alone
// includes this.

#include <assert.h>

#include "base/atomic.h"
#include "base/threading/mutex.h"
#include "foundation/algorithm/sort.h"
#include "foundation/math/scalar.h"

class RxVmaMutex {
 public:
  RxVmaMutex() = default;
  RxVmaMutex(const RxVmaMutex&) = delete;
  RxVmaMutex& operator=(const RxVmaMutex&) = delete;

  void Lock() { mutex_.lock(); }
  void Unlock() { mutex_.unlock(); }
  bool TryLock() { return mutex_.try_lock(); }

 private:
  base::Mutex mutex_;
};

class RxVmaRWMutex {
 public:
  void LockRead() { mutex_.lock_shared(); }
  void UnlockRead() { mutex_.unlock_shared(); }
  bool TryLockRead() { return mutex_.try_lock_shared(); }
  void LockWrite() { mutex_.lock(); }
  void UnlockWrite() { mutex_.unlock(); }
  bool TryLockWrite() { return mutex_.try_lock(); }

 private:
  base::SharedMutex mutex_;
};

#define VMA_MUTEX RxVmaMutex
#define VMA_RW_MUTEX RxVmaRWMutex
#define VMA_ATOMIC_UINT32 base::Atomic<uint32_t>
#define VMA_ATOMIC_UINT64 base::Atomic<uint64_t>
#define VMA_MIN(v1, v2) (rx::Min((v1), (v2)))
#define VMA_MAX(v1, v2) (rx::Max((v1), (v2)))
// VMA sorts only when defragmenting (block order by free size), which rx does
// not do; a stable sort keeps the result a function of the input if it ever
// does.
#define VMA_SORT(beg, end, cmp) (rx::StableSort((beg), (end), (cmp)))

#endif  // RX_RENDER_VULKAN_VMA_CONFIG_H_
