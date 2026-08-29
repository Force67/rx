#ifndef RX_WORLD_WORLD_OVERLAY_H_
#define RX_WORLD_WORLD_OVERLAY_H_

#include <span>
#include <string>

#include <base/containers/vector.h>

#include "core/export.h"
#include "core/math.h"
#include "core/types.h"

namespace rx::world {

// The third layer, and the only mutable one. A baked payload is immutable and
// shared between every save, every client and every patch; a player who breaks
// one fence must not cause the cell that fence was in to be rewritten. What
// changes lives here instead, sparsely, keyed by stable id.
//
// It is applied while a cell materializes, not afterwards: a destroyed row is
// skipped during the column copy rather than created and then destroyed, so
// the deletion is never briefly observable and the work is never done twice.
//
// This is deliberately small. It carries what a cell's own content can lose or
// move; quest state, inventories and anything a game owns belong to that game's
// save data, which refers to the same stable ids.

struct OverlayMove {
  u64 stable_id = 0;
  Vec3 position;
  Quat rotation;
  f32 scale = 1.0f;
};

class RX_WORLD_EXPORT WorldOverlay {
 public:
  // Which cook this overlay's stable ids mean something against. A stable id is
  // assigned by cook order, so the same id names a different row after a
  // re-bake: an overlay applied to the wrong world does not fail, it deletes
  // and moves the wrong things. Set it from WorldIndexData::bake_id before
  // serializing; WorldStreamer::SetOverlay refuses a mismatch. Zero means the
  // overlay is not keyed to any bake, which is only safe for one built and
  // discarded in memory.
  void set_bake_id(u64 id) { bake_id_ = id; }
  u64 bake_id() const { return bake_id_; }

  // Destroying an id also drops any move recorded for it: a row that is not
  // going to exist has no transform worth keeping.
  void Destroy(u64 stable_id);
  bool IsDestroyed(u64 stable_id) const;

  // False when the id is destroyed - moving it would resurrect it - or when the
  // transform is not finite. The decoder refuses a non-finite transform, so
  // accepting one here would let a transient simulation value produce a save
  // that writes successfully and then cannot be loaded.
  bool Move(u64 stable_id, Vec3 position, Quat rotation, f32 scale);
  const OverlayMove* FindMove(u64 stable_id) const;

  // Drops every delta for one id, putting it back to whatever the bake says.
  void Forget(u64 stable_id);
  void Clear();

  bool empty() const { return destroyed_.empty() && moves_.empty(); }
  size_t destroyed_count() const { return destroyed_.size(); }
  size_t move_count() const { return moves_.size(); }
  std::span<const u64> destroyed() const {
    return std::span<const u64>(destroyed_.data(), destroyed_.size());
  }
  std::span<const OverlayMove> moves() const {
    return std::span<const OverlayMove>(moves_.data(), moves_.size());
  }

  // Whether anything at all is recorded for a cell's stable-id range. The
  // streamer asks once per cell so the common case - a cell nobody has touched -
  // takes the bulk copy path rather than the row-by-row one.
  bool TouchesRange(u64 first, u32 count) const;

  bool Encode(base::Vector<u8>* out, std::string* error) const;
  static bool Decode(std::span<const u8> bytes, WorldOverlay* out, std::string* error);

 private:
  base::Vector<u64> destroyed_;      // sorted
  base::Vector<OverlayMove> moves_;  // sorted by stable id
  u64 bake_id_ = 0;
};

}  // namespace rx::world

#endif  // RX_WORLD_WORLD_OVERLAY_H_
