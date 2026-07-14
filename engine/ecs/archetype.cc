#include "ecs/archetype.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

namespace rx::ecs {

namespace {

u8* AllocateColumn(size_t byte_size, u32 align) {
  if (align > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
    return static_cast<u8*>(::operator new(byte_size, std::align_val_t(align)));
  return static_cast<u8*>(::operator new(byte_size));
}

void FreeColumn(u8* data, u32 align) {
  if (!data) return;
  if (align > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
    ::operator delete(data, std::align_val_t(align));
  else
    ::operator delete(data);
}

}  // namespace

Archetype::Column::~Column() { FreeColumn(data, align); }

Archetype::Column::Column(Column&& other) noexcept
    : id(other.id),
      stride(other.stride),
      align(other.align),
      data(std::exchange(other.data, nullptr)),
      capacity(std::exchange(other.capacity, 0)) {}

Archetype::Column& Archetype::Column::operator=(Column&& other) noexcept {
  if (this == &other) return *this;
  FreeColumn(data, align);
  id = other.id;
  stride = other.stride;
  align = other.align;
  data = std::exchange(other.data, nullptr);
  capacity = std::exchange(other.capacity, 0);
  return *this;
}

void Archetype::Column::Reserve(u32 row_capacity, u32 live_rows) {
  if (row_capacity <= capacity) return;

  size_t new_capacity = capacity > 0 ? capacity * 2 : 1;
  if (new_capacity < row_capacity) new_capacity = row_capacity;
  u8* new_data = AllocateColumn(new_capacity * static_cast<size_t>(stride), align);
  const ComponentInfo& info = GetComponentInfo(id);
  for (u32 row = 0; row < live_rows; ++row) {
    u8* source = data + static_cast<size_t>(row) * stride;
    info.move_construct(new_data + static_cast<size_t>(row) * stride, source);
    info.destruct(source);
  }
  FreeColumn(data, align);
  data = new_data;
  capacity = new_capacity;
}

Archetype::Archetype(Signature signature) : signature_(std::move(signature)) {
  columns_.reserve(signature_.size());
  for (ComponentId id : signature_) {
    const ComponentInfo& info = GetComponentInfo(id);
    columns_.emplace_back(id, info.size, info.align);
  }
}

Archetype::~Archetype() {
  for (auto& column : columns_) {
    const ComponentInfo& info = GetComponentInfo(column.id);
    for (u32 row = 0; row < row_count(); ++row) {
      info.destruct(column.data + static_cast<size_t>(row) * column.stride);
    }
  }
}

u32 Archetype::AddRow(Entity entity) {
  u32 row = row_count();
  entities_.push_back(entity);
  for (auto& column : columns_) {
    column.Reserve(row + 1, row);
  }
  return row;
}

Entity Archetype::SwapRemoveRow(u32 row) {
  u32 last = row_count() - 1;
  for (auto& column : columns_) {
    const ComponentInfo& info = GetComponentInfo(column.id);
    u8* dst = column.data + static_cast<size_t>(row) * column.stride;
    info.destruct(dst);
    if (row != last) {
      u8* src = column.data + static_cast<size_t>(last) * column.stride;
      info.move_construct(dst, src);
      info.destruct(src);
    }
  }
  Entity moved = kInvalidEntity;
  if (row != last) {
    moved = entities_[last];
    entities_[row] = moved;
  }
  entities_.pop_back();
  return moved;
}

void* Archetype::ComponentAt(ComponentId id, u32 row) {
  int index = ColumnIndex(id);
  if (index < 0) return nullptr;
  Column& column = columns_[static_cast<size_t>(index)];
  return column.data + static_cast<size_t>(row) * column.stride;
}

void* Archetype::ColumnData(ComponentId id) {
  int index = ColumnIndex(id);
  return index < 0 ? nullptr : columns_[static_cast<size_t>(index)].data;
}

int Archetype::ColumnIndex(ComponentId id) const {
  auto it = std::lower_bound(signature_.begin(), signature_.end(), id);
  if (it == signature_.end() || *it != id) return -1;
  return static_cast<int>(it - signature_.begin());
}

}  // namespace rx::ecs
