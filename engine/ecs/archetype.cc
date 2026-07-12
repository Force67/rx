#include "ecs/archetype.h"

#include <algorithm>
#include <cstring>

namespace rx::ecs {

Archetype::Archetype(Signature signature) : signature_(std::move(signature)) {
  columns_.reserve(signature_.size());
  for (ComponentId id : signature_) {
    columns_.push_back(Column{.id = id, .stride = GetComponentInfo(id).size, .data = {}});
  }
}

Archetype::~Archetype() {
  for (auto& column : columns_) {
    const ComponentInfo& info = GetComponentInfo(column.id);
    for (u32 row = 0; row < row_count(); ++row) {
      info.destruct(column.data.data() + static_cast<size_t>(row) * column.stride);
    }
  }
}

u32 Archetype::AddRow(Entity entity) {
  u32 row = row_count();
  entities_.push_back(entity);
  for (auto& column : columns_) {
    const size_t old_bytes = static_cast<size_t>(row) * column.stride;
    const size_t new_bytes = old_bytes + column.stride;
    // A plain resize would bitwise-relocate the existing rows when the byte
    // buffer reallocates. That is fine for trivially-relocatable components but
    // corrupts ones whose bytes reference themselves (e.g. an SSO std::string).
    // On a reallocating growth, relocate each existing element through its
    // registered move_construct/destruct instead of the raw byte copy.
    if (new_bytes > column.data.capacity()) {
      const ComponentInfo& info = GetComponentInfo(column.id);
      base::Vector<u8> fresh;
      fresh.reserve(std::max<size_t>(new_bytes, column.data.capacity() * 2));
      fresh.resize(new_bytes);
      for (u32 r = 0; r < row; ++r) {
        u8* src = column.data.data() + static_cast<size_t>(r) * column.stride;
        info.move_construct(fresh.data() + static_cast<size_t>(r) * column.stride, src);
        info.destruct(src);
      }
      column.data = std::move(fresh);
    } else {
      column.data.resize(new_bytes);  // same buffer: existing rows stay put
    }
  }
  return row;
}

Entity Archetype::SwapRemoveRow(u32 row) {
  u32 last = row_count() - 1;
  for (auto& column : columns_) {
    const ComponentInfo& info = GetComponentInfo(column.id);
    u8* dst = column.data.data() + static_cast<size_t>(row) * column.stride;
    info.destruct(dst);
    if (row != last) {
      u8* src = column.data.data() + static_cast<size_t>(last) * column.stride;
      info.move_construct(dst, src);
      info.destruct(src);
    }
    column.data.resize(column.data.size() - column.stride);
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
  return column.data.data() + static_cast<size_t>(row) * column.stride;
}

void* Archetype::ColumnData(ComponentId id) {
  int index = ColumnIndex(id);
  return index < 0 ? nullptr : columns_[static_cast<size_t>(index)].data.data();
}

int Archetype::ColumnIndex(ComponentId id) const {
  auto it = std::lower_bound(signature_.begin(), signature_.end(), id);
  if (it == signature_.end() || *it != id) return -1;
  return static_cast<int>(it - signature_.begin());
}

}  // namespace rx::ecs
