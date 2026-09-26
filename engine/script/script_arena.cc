#include "base/check.h"
#include "base/memory/mem_ops.h"
#include "base/memory/unique_pointer.h"
#include "base/numeric_limits.h"
#include "script/script_arena.h"

#include <stdint.h>
#include <string.h>

namespace rx::script {

namespace {
size_t AlignUp(size_t n, size_t align) {
  const size_t padding = align - 1;
  BASE_FATAL_CHECK(n <= SIZE_MAX - padding, "script arena request overflows size_t");
  return (n + padding) & ~padding;
}
}  // namespace

ScriptArena::ScriptArena(size_t block_size) : block_size_(block_size) {}

ScriptArena::~ScriptArena() {
  Block* const lists[] = {head_, free_list_};
  for (Block* b : lists) {
    while (b) {
      Block* next = b->next;
      delete[] b->base;
      delete b;
      b = next;
    }
  }
}

ScriptArena::Block* ScriptArena::NewBlock(size_t min_bytes) {
  // Reuse a Reset() block if one is large enough, else allocate.
  Block** link = &free_list_;
  while (*link) {
    if ((*link)->cap >= min_bytes) {
      Block* b = *link;
      *link = b->next;
      b->used = 0;
      b->next = nullptr;
      return b;
    }
    link = &(*link)->next;
  }
  size_t cap = min_bytes > block_size_ ? min_bytes : block_size_;
  Block* b = new Block();
  b->base = new u8[cap];
  b->cap = cap;
  b->used = 0;
  b->next = nullptr;
  return b;
}

void* ScriptArena::Alloc(size_t bytes, size_t align) {
  // Blocks come from new[], aligned to the default new alignment; requests up to
  // that (covers every script POD, incl. ScriptValue) get correct alignment
  // relative to the block base. Over-aligned requests are not supported.
  BASE_FATAL_CHECK(align != 0 && (align & (align - 1)) == 0 && align <= alignof(max_align_t),
                "script arena alignment must be a power of two up to max_align_t");
  BASE_FATAL_CHECK(bytes <= SIZE_MAX - used_, "script arena request overflows size_t");
  const size_t min_block_bytes = AlignUp(bytes, align);

  size_t off = head_ ? AlignUp(head_->used, align) : 0;
  if (!head_ || off > head_->cap || bytes > head_->cap - off) {
    Block* b = NewBlock(min_block_bytes);
    b->next = head_;
    head_ = b;
    off = 0;
  }
  head_->used = off + bytes;
  used_ += bytes;
  if (used_ > high_water_) high_water_ = used_;
  return head_->base + off;
}

void ScriptArena::Reset() {
  // Move every live block onto the free list, keeping their storage for reuse.
  while (head_) {
    Block* next = head_->next;
    head_->used = 0;
    head_->next = free_list_;
    free_list_ = head_;
    head_ = next;
  }
  used_ = 0;
}

ScriptStringView ArenaCopy(ScriptArena& arena, ScriptStringView s) {
  BASE_FATAL_CHECK(s.size != base::MinMax<u32>::max(), "script string longer than u32");
  char* dst = static_cast<char*>(
      arena.Alloc(static_cast<size_t>(s.size) + 1, alignof(char)));
  if (s.size) base::MemCopy(dst, s.data, s.size);
  dst[s.size] = '\0';
  return ScriptStringView(dst, s.size);
}

}  // namespace rx::script
