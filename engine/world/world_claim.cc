#include "world/world_claim.h"

#include <algorithm>

namespace rx::world {

const char* ClaimKindName(ClaimKind kind) {
  switch (kind) {
    case ClaimKind::kHard: return "hard";
    case ClaimKind::kSoft: return "soft";
    case ClaimKind::kSpeculative: return "speculative";
  }
  return "unknown";
}

u64 ClaimSet::Add(const ResidencyClaim& claim) {
  const u64 handle = next_handle_++;
  ResidencyClaim stored = claim;
  if (stored.reason == nullptr) stored.reason = "";
  entries_.push_back({handle, stored});
  return handle;
}

bool ClaimSet::Remove(u64 handle) {
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].handle != handle) continue;
    entries_.erase(entries_.begin() + i);
    return true;
  }
  return false;
}

size_t ClaimSet::RemoveOwner(u64 owner) {
  size_t removed = 0;
  for (size_t i = 0; i < entries_.size();) {
    if (entries_[i].claim.owner == owner) {
      entries_.erase(entries_.begin() + i);
      ++removed;
    } else {
      ++i;
    }
  }
  return removed;
}

size_t ClaimSet::Expire(u64 tick) {
  size_t removed = 0;
  for (size_t i = 0; i < entries_.size();) {
    const ResidencyClaim& claim = entries_[i].claim;
    if (claim.expires_at_tick != 0 && tick >= claim.expires_at_tick) {
      entries_.erase(entries_.begin() + i);
      ++removed;
    } else {
      ++i;
    }
  }
  return removed;
}

void ClaimSet::Clear() { entries_.clear(); }

void ClaimSet::set_weakest_honored(ClaimKind kind) {
  weakest_honored_ = kind < ClaimKind::kHard ? ClaimKind::kHard : kind;
}

bool ClaimSet::Holds(u64 cell, u32 domains) const {
  for (const ClaimEntry& entry : entries_) {
    if (entry.claim.cell != cell || (entry.claim.domains & domains) == 0) continue;
    if (Honors(entry.claim)) return true;
  }
  return false;
}

i32 ClaimSet::Priority(u64 cell, u32 domains) const {
  i32 priority = 0;
  for (const ClaimEntry& entry : entries_) {
    if (entry.claim.cell != cell || (entry.claim.domains & domains) == 0) continue;
    if (!Honors(entry.claim)) continue;
    // Hard outranks soft outranks speculative, and any claim outranks none.
    const i32 candidate = entry.claim.kind == ClaimKind::kHard      ? 3
                          : entry.claim.kind == ClaimKind::kSoft    ? 2
                                                                    : 1;
    priority = std::max(priority, candidate);
  }
  return priority;
}

void ClaimSet::Explain(u64 cell, u32 domains, base::Vector<ResidencyClaim>* out) const {
  if (!out) return;
  out->clear();
  for (const ClaimEntry& entry : entries_) {
    if (entry.claim.cell != cell || (entry.claim.domains & domains) == 0) continue;
    if (!Honors(entry.claim)) continue;
    out->push_back(entry.claim);
  }
  std::sort(out->begin(), out->end(), [](const ResidencyClaim& a, const ResidencyClaim& b) {
    if (a.kind != b.kind) return a.kind < b.kind;
    return a.owner < b.owner;
  });
}

}  // namespace rx::world
