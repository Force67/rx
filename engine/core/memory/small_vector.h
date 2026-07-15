#ifndef RX_CORE_MEMORY_SMALL_VECTOR_H_
#define RX_CORE_MEMORY_SMALL_VECTOR_H_

#include <cstddef>
#include <limits>
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

  SmallVector(SmallVector&& other) noexcept(std::is_nothrow_move_constructible_v<T>) {
    MoveFrom(std::move(other));
  }

  SmallVector& operator=(SmallVector&& other) noexcept(std::is_nothrow_move_constructible_v<T>) {
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
    if (size_ == capacity_) return GrowAndEmplace(std::forward<Args>(args)...);
    T* slot = data_ + size_;
    new (slot) T(std::forward<Args>(args)...);
    ++size_;
    return *slot;
  }

  void pop_back() { data_[--size_].~T(); }

  void resize(size_t new_size) {
    reserve(new_size);
    while (size_ < new_size) {
      new (data_ + size_) T();
      ++size_;
    }
    while (size_ > new_size) pop_back();
  }

  void clear() {
    for (size_t i = 0; i < size_; ++i) data_[i].~T();
    size_ = 0;
  }

 private:
  bool IsInline() const { return data_ == reinterpret_cast<const T*>(inline_storage_); }

  static constexpr size_t MaxCapacity() {
    return std::numeric_limits<size_t>::max() / sizeof(T);
  }

  static T* Allocate(size_t capacity) {
    if (capacity > MaxCapacity()) throw std::bad_array_new_length();
    const size_t bytes = capacity * sizeof(T);
    if constexpr (alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
      return static_cast<T*>(::operator new(bytes, std::align_val_t{alignof(T)}));
    } else {
      return static_cast<T*>(::operator new(bytes));
    }
  }

  static void Deallocate(T* pointer) {
    if constexpr (alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
      ::operator delete(pointer, std::align_val_t{alignof(T)});
    } else {
      ::operator delete(pointer);
    }
  }

  size_t GrowthCapacity(size_t requested) const {
    if (requested > MaxCapacity()) throw std::bad_array_new_length();
    const size_t doubled = capacity_ > MaxCapacity() / 2 ? MaxCapacity() : capacity_ * 2;
    return requested < doubled ? doubled : requested;
  }

  void Grow(size_t new_capacity) {
    new_capacity = GrowthCapacity(new_capacity);
    T* block = Allocate(new_capacity);
    size_t constructed = 0;
    try {
      for (; constructed < size_; ++constructed) {
        new (block + constructed) T(std::move_if_noexcept(data_[constructed]));
      }
    } catch (...) {
      for (size_t i = 0; i < constructed; ++i) block[i].~T();
      Deallocate(block);
      throw;
    }
    for (size_t i = 0; i < size_; ++i) data_[i].~T();
    if (!IsInline()) Deallocate(data_);
    data_ = block;
    capacity_ = new_capacity;
  }

  template <typename... Args>
  T& GrowAndEmplace(Args&&... args) {
    if (capacity_ == MaxCapacity()) throw std::bad_array_new_length();
    const size_t new_capacity = GrowthCapacity(capacity_ + 1);
    T* block = Allocate(new_capacity);

    // Construct the appended value while aliased arguments still refer to the
    // old storage, then relocate the existing prefix.
    try {
      new (block + size_) T(std::forward<Args>(args)...);
    } catch (...) {
      Deallocate(block);
      throw;
    }
    size_t constructed = 0;
    try {
      for (; constructed < size_; ++constructed) {
        new (block + constructed) T(std::move_if_noexcept(data_[constructed]));
      }
    } catch (...) {
      for (size_t i = 0; i < constructed; ++i) block[i].~T();
      block[size_].~T();
      Deallocate(block);
      throw;
    }

    for (size_t i = 0; i < size_; ++i) data_[i].~T();
    if (!IsInline()) Deallocate(data_);
    data_ = block;
    capacity_ = new_capacity;
    return data_[size_++];
  }

  void MoveFrom(SmallVector&& other) {
    if (other.IsInline()) {
      data_ = reinterpret_cast<T*>(inline_storage_);
      capacity_ = N;
      size_ = 0;
      try {
        for (; size_ < other.size_; ++size_) {
          new (data_ + size_) T(std::move(other.data_[size_]));
        }
      } catch (...) {
        clear();
        throw;
      }
      for (size_t i = 0; i < other.size_; ++i) other.data_[i].~T();
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
    if (!IsInline()) Deallocate(data_);
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
