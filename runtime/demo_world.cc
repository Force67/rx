#include "demo_world.h"

#include <cmath>

#include "asset/pack.h"
#include "asset/primitives.h"
#include "core/log.h"
#include "scene/components.h"

namespace rx {
namespace {

// The prototypes the baked world names, and the meshes they resolve to. A
// cooked world stores a name, not a mesh: the archive has no idea what a
// "world/pillar" looks like, and the game decides. Registering them up front
// means a cell that streams in never waits on an asset.
struct Prototype {
  const char* name;
  Vec3 half_extent;
};

constexpr Prototype kPrototypes[] = {
    {"world/tile", {16.0f, 0.25f, 16.0f}},  // one ground tile per 32 m cell
    {"world/pillar", {1.0f, 4.0f, 1.0f}},
    {"world/scatter", {0.6f, 0.6f, 0.6f}},
};

// How far each domain streams, for a world of 32 m cells. Gameplay reaches
// less far than representation on purpose: a missing pillar is a missing
// thing, a missing tile is a hole in the floor, and the eye finds the hole
// first.
world::WorldStreamPolicy DemoPolicy() {
  world::WorldStreamPolicy policy;
  for (u32 i = 0; i < world::kDomainCount; ++i) policy.domains[i].load_distance = 0;

  world::DomainStreamPolicy& gameplay = policy[world::Domain::kGameplay];
  gameplay.load_distance = 70.0f;
  gameplay.retain_distance = 95.0f;
  gameplay.rows_per_commit = 64;

  world::DomainStreamPolicy& representation = policy[world::Domain::kRepresentation];
  representation.load_distance = 110.0f;
  representation.retain_distance = 140.0f;
  representation.rows_per_commit = 256;
  return policy;
}

}  // namespace

void WorldStreamDemo::RegisterMeshes(render::Renderer* renderer, bool headless) {
  for (const Prototype& prototype : kPrototypes) {
    asset::Mesh mesh = asset::MakeBox(prototype.half_extent.x, prototype.half_extent.y,
                                      prototype.half_extent.z, asset::MakeAssetId(prototype.name));
    if (!headless && renderer != nullptr) renderer->UploadMesh(mesh);
  }
}

bool WorldStreamDemo::Init(asset::Vfs& vfs, render::Renderer* renderer, ecs::World& ecs,
                           bool headless, const std::string& archive_path,
                           const std::string& world_name) {
  world_name_ = world_name;

  auto provider = asset::MakePackFileProvider(archive_path);
  if (!provider) {
    RX_ERROR("--world {}: not a readable .rxp archive", archive_path);
    return false;
  }
  vfs.Mount("world", std::move(provider));

  const std::string index_path = "world://" + world_name + "/" + world_name + ".rxworld";
  std::string error;
  if (!map_.Load(vfs, index_path, &error)) {
    RX_ERROR("--world {}: {}", archive_path, error);
    return false;
  }

  RegisterMeshes(renderer, headless);

  loader_ = world::MakeArchiveCellLoader(map_, vfs);
  streamer_ = base::MakeUnique<world::WorldStreamer>(map_, *loader_, ecs);
  streamer_->Configure(DemoPolicy());
  streamer_->SetClaims(&claims_);

  // The cell the camera starts in, claimed hard so the first frame is not a
  // hole while the ordinary radius work catches up. A real game would claim the
  // spawn point, and drop it once the player has arrived.
  if (!map_.index().cells.empty()) {
    claims_.Add({/*owner=*/1, map_.index().cells[0].id, ~u32{0}, world::ClaimKind::kHard,
                 /*expires_at_tick=*/0, /*full_detail=*/true, "the spawn cell"});
  }

  RX_INFO("--world {}: {} cells, bake {}", archive_path, map_.index().cells.size(),
          map_.index().bake_id);
  return true;
}

void WorldStreamDemo::Update(f32 frame_delta, const Vec3& camera_position) {
  if (!streamer_) return;

  scene::WorldStreamObservation observer;
  observer.position = camera_position;
  if (have_previous_ && frame_delta > 0.0f) {
    observer.velocity = {(camera_position.x - previous_position_.x) / frame_delta,
                         (camera_position.y - previous_position_.y) / frame_delta,
                         (camera_position.z - previous_position_.z) / frame_delta};
  }
  previous_position_ = camera_position;
  have_previous_ = true;
  // Look far enough ahead that a cell is asked for before it is needed, and
  // cap it so a teleport does not request a corridor across the whole world.
  observer.prediction_seconds = 1.5f;
  observer.maximum_prediction_distance = 60.0f;
  // The world is a plane; height should not decide what is resident.
  observer.axes = scene::kWorldStreamXZ;

  streamer_->Update(std::span<const scene::WorldStreamObservation>(&observer, 1));

  // A refused payload is a cook or archive bug, so say so once rather than
  // every frame for as long as the cell stays in range.
  const std::span<const std::string> errors = streamer_->errors();
  for (size_t i = reported_errors_; i < errors.size(); ++i) RX_ERROR("world: {}", errors[i]);
  reported_errors_ = errors.size();
}

void WorldStreamDemo::EmitToView(render::FrameView& view) {
  if (!streamer_) return;

  streamer_->ResidentCells(world::Domain::kRepresentation, &resident_scratch_);
  for (u64 cell : resident_scratch_) {
    const std::span<const world::ResidentInstance> instances = streamer_->Instances(cell);
    if (instances.empty()) continue;

    // The page stores a prototype index; the names it indexes are the cell's,
    // and the mesh each resolves to is this host's business.
    const std::span<const std::string> prototypes = streamer_->Prototypes(cell);
    prototype_ids_scratch_.clear();
    prototype_ids_scratch_.reserve(prototypes.size());
    for (const std::string& name : prototypes) {
      prototype_ids_scratch_.push_back(asset::MakeAssetId(name).hash);
    }

    for (const world::ResidentInstance& instance : instances) {
      // A promoted row now has an entity of its own, and the host draws that.
      // Drawing the page row too would put two rocks in one place.
      if (instance.promoted) continue;
      if (instance.prototype >= prototype_ids_scratch_.size()) continue;
      const Mat4 transform =
          MakeTransform(instance.position, instance.rotation, instance.scale);
      // Static: the previous transform is this one, so the motion vector is
      // zero rather than a smear from wherever the page last was.
      view.draws.push_back(
          render::DrawItem{prototype_ids_scratch_[instance.prototype], transform, transform});
    }
  }
}

void WorldStreamDemo::Shutdown() {
  if (!streamer_) return;
  streamer_->Shutdown();
  streamer_.Reset();
  loader_.Reset();
}

std::string WorldStreamDemo::StatusLine() const {
  if (!streamer_) return {};
  const world::WorldStreamerStats stats = streamer_->stats();
  // Resident is (cell, domain) pairs, not cells: the whole point is that a
  // cell's domains come and go independently, so one number for "cells" would
  // be the wrong shape.
  std::string line = "WORLD " + std::to_string(stats.resident) + "/" +
                     std::to_string(map_.index().cells.size() * 2) + " res";
  if (stats.pending != 0) line += " +" + std::to_string(stats.pending);
  line += "  " + std::to_string(stats.entities) + "e " + std::to_string(stats.instances) + "i  " +
          std::to_string(stats.resident_bytes / 1024) + "K";
  if (stats.suppressed != 0) line += "  " + std::to_string(stats.suppressed) + " SUPPRESSED";
  if (stats.errors_total != 0) line += "  " + std::to_string(stats.errors_total) + " ERR";
  return line;
}

}  // namespace rx
