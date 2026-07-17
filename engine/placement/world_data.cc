#include "placement/world_data.h"

#include <algorithm>
#include <cmath>

namespace rx::placement {

WorldData::WorldData(f32 origin_x, f32 origin_z, f32 extent_m, u32 resolution)
    : origin_x_(origin_x), origin_z_(origin_z), extent_(extent_m), resolution_(resolution) {}

u32 WorldData::AddMap(std::string name, f32 fill) {
  Map map;
  map.name = std::move(name);
  map.texels.resize(static_cast<std::size_t>(resolution_) * resolution_, fill);
  maps_.push_back(std::move(map));
  revisions_.push_back(1);
  return static_cast<u32>(maps_.size() - 1);
}

std::span<f32> WorldData::texels(u32 map) {
  return {maps_[map].texels.data(), maps_[map].texels.size()};
}

std::span<const f32> WorldData::texels(u32 map) const {
  return {maps_[map].texels.data(), maps_[map].texels.size()};
}

f32& WorldData::At(u32 map, u32 x, u32 z) {
  return maps_[map].texels[static_cast<std::size_t>(z) * resolution_ + x];
}

f32 WorldData::Sample(u32 map, f32 world_x, f32 world_z) const {
  // Texel centers sit at (i + 0.5) * step; this is the CPU mirror of a
  // linear/clamp sampler over the uploaded texture array.
  f32 step = meters_per_texel();
  f32 u = (world_x - origin_x_) / step - 0.5f;
  f32 v = (world_z - origin_z_) / step - 0.5f;
  f32 fu = std::floor(u);
  f32 fv = std::floor(v);
  f32 tu = u - fu;
  f32 tv = v - fv;
  i32 max_texel = static_cast<i32>(resolution_) - 1;
  auto texel = [&](i32 x, i32 z) {
    x = std::clamp(x, 0, max_texel);
    z = std::clamp(z, 0, max_texel);
    return maps_[map].texels[static_cast<std::size_t>(z) * resolution_ + static_cast<std::size_t>(x)];
  };
  i32 x0 = static_cast<i32>(fu);
  i32 z0 = static_cast<i32>(fv);
  f32 a = texel(x0, z0) + (texel(x0 + 1, z0) - texel(x0, z0)) * tu;
  f32 b = texel(x0, z0 + 1) + (texel(x0 + 1, z0 + 1) - texel(x0, z0 + 1)) * tu;
  return a + (b - a) * tv;
}

void WorldData::PaintDisc(u32 map, f32 center_x, f32 center_z, f32 radius, f32 value) {
  if (radius <= 0.0f) return;
  f32 step = meters_per_texel();
  i32 max_texel = static_cast<i32>(resolution_) - 1;
  i32 x0 = std::clamp(static_cast<i32>((center_x - radius - origin_x_) / step), 0, max_texel);
  i32 x1 = std::clamp(static_cast<i32>((center_x + radius - origin_x_) / step) + 1, 0, max_texel);
  i32 z0 = std::clamp(static_cast<i32>((center_z - radius - origin_z_) / step), 0, max_texel);
  i32 z1 = std::clamp(static_cast<i32>((center_z + radius - origin_z_) / step) + 1, 0, max_texel);
  for (i32 z = z0; z <= z1; ++z) {
    for (i32 x = x0; x <= x1; ++x) {
      f32 wx = origin_x_ + (static_cast<f32>(x) + 0.5f) * step;
      f32 wz = origin_z_ + (static_cast<f32>(z) + 0.5f) * step;
      f32 d = std::sqrt((wx - center_x) * (wx - center_x) + (wz - center_z) * (wz - center_z));
      if (d >= radius) continue;
      f32 strength = std::min(1.0f, (radius - d) / (radius * 0.25f + 1e-5f));
      f32& texel = At(map, static_cast<u32>(x), static_cast<u32>(z));
      texel = texel + (value - texel) * strength;
    }
  }
  ++revisions_[map];
}

}  // namespace rx::placement
