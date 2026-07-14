#ifndef RX_CORE_MEMORY_SMALL_VECTOR_H_
#define RX_CORE_MEMORY_SMALL_VECTOR_H_

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

#include "core/types.h"

namespace rx::mem {

// Vector with N elements of inline storage: no heap traffic until the size
// exceeds N. For the hot-path scratch lists that are almost always tiny
// (descriptor writes, signature builds) where a heap-backed vector would
// allocate and free every call. Move-only: the use cases are scratch and
// return values, and copying would hide exactly the allocations this type
// exists to avoid.
template <typename T, size_t N>
class SmallVector {
  static_assert(N > 0);

 public:
  SmallVector() = default;

  SmallVector(SmallVector&& other) noexcept { MoveFrom(std::move(other)); }

  SmallVector& operator=(SmallVector&& other) noexcept {
    if (this != &other) {
      Destroy();
      MoveFrom(std::move(other));
    }
    return *this;
  }

  SmallVector(const SmallVector&) = delete;
  SmallVector& operator=(const SmallVector&) = delete;

  ~SmallVector() { Destroy(); }

  T* data() { return data_; }
  const T* data() const { return data_; }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  size_t capacity() const { return capacity_; }

  T* begin() { return data_; }
  T* end() { return data_ + size_; }
  const T* begin() const { return data_; }
  const T* end() const { return data_ + size_; }

  T& operator[](size_t index) { return data_[index]; }
  const T& operator[](size_t index) const { return data_[index]; }
  T& back() { return data_[size_ - 1]; }

  void reserve(size_t capacity) {
    if (capacity > capacity_) Grow(capacity);
  }

  T& push_back(T value) { return emplace_back(std::move(value)); }

  template <typename... Args>
  T& emplace_back(Args&&... args) {
    if (size_ == capacity_) Grow(capacity_ * 2);
    return *new (data_ + size_++) T(std::forward<Args>(args)...);
  }

  void pop_back() { data_[--size_].~T(); }

  void resize(size_t new_size) {
    reserve(new_size);
    while (size_ < new_size) new (data_ + size_++) T();
    while (size_ > new_size) pop_back();
  }

  void clear() {
    for (size_t i = 0; i < size_; ++i) data_[i].~T();
    size_ = 0;
  }

 private:
  bool IsInline() const { return data_ == reinterpret_cast<const T*>(inline_storage_); }

  void Grow(size_t new_capacity) {
    if (new_capacity < capacity_ * 2) new_capacity = capacity_ * 2;
    T* block = static_cast<T*>(::operator new(new_capacity * sizeof(T), std::align_val_t{alignof(T)}));
    for (size_t i = 0; i < size_; ++i) {
      new (block + i) T(std::move(data_[i]));
      data_[i].~T();
    }
    if (!IsInline()) ::operator delete(data_, std::align_val_t{alignof(T)});
    data_ = block;
    capacity_ = new_capacity;
  }

  void MoveFrom(SmallVector&& other) {
    if (other.IsInline()) {
      data_ = reinterpret_cast<T*>(inline_storage_);
      capacity_ = N;
      size_ = other.size_;
      for (size_t i = 0; i < size_; ++i) {
        new (data_ + i) T(std::move(other.data_[i]));
        other.data_[i].~T();
      }
      other.size_ = 0;
    } else {
      data_ = other.data_;
      size_ = other.size_;
      capacity_ = other.capacity_;
      other.data_ = reinterpret_cast<T*>(other.inline_storage_);
      other.size_ = 0;
      other.capacity_ = N;
    }
  }

  void Destroy() {
    clear();
    if (!IsInline()) ::operator delete(data_, std::align_val_t{alignof(T)});
    data_ = reinterpret_cast<T*>(inline_storage_);
    capacity_ = N;
  }

  alignas(T) unsigned char inline_storage_[N * sizeof(T)];
  T* data_ = reinterpret_cast<T*>(inline_storage_);
  size_t size_ = 0;
  size_t capacity_ = N;
};

}  // namespace rx::mem

#endif  // RX_CORE_MEMORY_SMALL_VECTOR_H_
