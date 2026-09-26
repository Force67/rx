#ifndef RX_CORE_SHARED_H_
#define RX_CORE_SHARED_H_

#include "base/atomic.h"
#include "base/memory/move.h"
#include "core/types.h"

namespace rx {

// Shared ownership of one heap object: copies share it and the last one to go
// destroys it. base has no shared pointer; the engine needs one only where two
// lifetimes cross and either may end first (a synth voice's parameter mailbox,
// shared by the engine thread and the audio thread; a demo's alive flag, held
// by closures a scheduler keeps after the demo is gone).
//
// Counting follows shared_ptr: an acquire is relaxed (a holder already
// exists), a release is acq_rel (every use through any holder happens before
// the delete). The object itself is not synchronized.
template <typename T>
class Shared {
 public:
  Shared() = default;

  template <typename... Args>
  static Shared Make(Args&&... args) {
    return Shared(new Block(base::forward<Args>(args)...));
  }

  Shared(const Shared& other) : block_(other.block_) {
    if (block_) block_->refs.fetch_add(1, base::memory_order_relaxed);
  }
  Shared(Shared&& other) noexcept : block_(other.block_) { other.block_ = nullptr; }
  Shared& operator=(Shared other) noexcept {
    Block* old = block_;
    block_ = other.block_;
    other.block_ = old;  // released when `other` goes out of scope
    return *this;
  }
  ~Shared() {
    if (block_ && block_->refs.fetch_sub(1, base::memory_order_acq_rel) == 1) delete block_;
  }

  T* operator->() const { return &block_->value; }
  T& operator*() const { return block_->value; }
  explicit operator bool() const { return block_ != nullptr; }

 private:
  struct Block {
    template <typename... Args>
    explicit Block(Args&&... args) : value(base::forward<Args>(args)...) {}
    base::Atomic<u32> refs{1};
    T value;
  };

  explicit Shared(Block* block) : block_(block) {}

  Block* block_ = nullptr;
};

}  // namespace rx

#endif  // RX_CORE_SHARED_H_
