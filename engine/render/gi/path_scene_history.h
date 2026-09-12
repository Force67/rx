#ifndef RX_RENDER_PATH_SCENE_HISTORY_H_
#define RX_RENDER_PATH_SCENE_HISTORY_H_

#include <cstring>
#include <span>

#include "render/gi/raytracing.h"

namespace rx::render {

class PathSceneHistory {
 public:
  bool Update(base::Vector<RayTracingContext::Instance>& instances,
              std::span<const Mat4> bones) {
    bool changed = instances.size() != previous_.size() || bones.size() != bones_.size();
    if (!changed && !bones.empty())
      changed = std::memcmp(bones.data(), bones_.data(), bones.size_bytes()) != 0;
    if (instances.size() > 0xfffffffeu - next_id_) {
      previous_.clear();
      next_id_ = 0;
      changed = true;
    }
    for (size_t i = 0; i < instances.size(); ++i) {
      auto& current = instances[i];
      const bool same = i < previous_.size() && SameGeometry(current, previous_[i]);
      changed |= !same || !SameTransform(current.transform, previous_[i].transform) ||
                 (current.skinned && current.previous_mesh == 0xffffffffu);
      const bool valid = same && current.previous_mesh != 0xffffffffu && SameTransform(current.previous_transform, previous_[i].transform);
      current.history_id = valid ? previous_[i].history_id : next_id_++;
      if (!valid) current.previous_mesh = 0xffffffffu;
    }
    previous_ = instances;
    bones_.assign(bones.begin(), bones.end());
    return changed;
  }

 private:
  static bool SameTransform(const Mat4& a, const Mat4& b) {
    return std::memcmp(&a, &b, sizeof(Mat4)) == 0;
  }
  static bool SameGeometry(const RayTracingContext::Instance& a,
                            const RayTracingContext::Instance& b) {
    const u64 ak = a.skinned ? a.mesh_key >> 1 : a.mesh_key;
    const u64 bk = b.skinned ? b.mesh_key >> 1 : b.mesh_key;
    return ak == bk && a.skinned == b.skinned && a.mask == b.mask &&
           a.approx == b.approx && a.lod == b.lod &&
           (a.skinned || a.custom_index == b.custom_index);
  }

  base::Vector<RayTracingContext::Instance> previous_;
  base::Vector<Mat4> bones_;
  u32 next_id_ = 0;
};

}  // namespace rx::render

#endif  // RX_RENDER_PATH_SCENE_HISTORY_H_
