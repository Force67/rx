#include "script/script_arena.h"

#include <cassert>
#include <cstring>
#include <new>

namespace rx::script {

namespace {
size_t AlignUp(size_t n, size_t align) { return (n + align - 1) & ~(align - 1); }
}  // namespace

ScriptArena::ScriptArena(size_t block_size) : block_size_(block_size) {}

ScriptArena::~ScriptArena() {
  for (Block* b : {head_, free_list_}) {
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
  Block* b = new Block;
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
  assert(align <= alignof(std::max_align_t) && "ScriptArena: over-aligned request");
  if (!head_ || AlignUp(head_->used, align) + bytes > head_->cap) {
    Block* b = NewBlock(AlignUp(bytes, align));
    b->next = head_;
    head_ = b;
  }
  size_t off = AlignUp(head_->used, align);
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
  char* dst = static_cast<char*>(arena.Alloc(s.size + 1, alignof(char)));
  if (s.size) std::memcpy(dst, s.data, s.size);
  dst[s.size] = '\0';
  return ScriptStringView(dst, s.size);
}

}  // namespace rx::script
