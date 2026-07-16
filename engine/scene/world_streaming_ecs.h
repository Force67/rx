#ifndef RX_SCENE_WORLD_STREAMING_ECS_H_
#define RX_SCENE_WORLD_STREAMING_ECS_H_

#include "ecs/world.h"
#include "scene/components.h"
#include "scene/world_streaming.h"

namespace rx::scene {

// Attach to a parent-free Transform to make an entity a streaming source.
// Content enters at load_distance and remains requested or resident until it
// leaves the larger retain_distance. Prediction adds a conservative swept
// volume ahead of velocity; it never removes the omnidirectional baseline.
struct WorldStreamObserver {
  Vec3 velocity;
  f32 load_distance = 0;
  f32 retain_distance = 0;
  f32 prediction_seconds = 0;
  f32 maximum_prediction_distance = 0;
  u32 channels = ~u32{0};
  u8 axes = kWorldStreamXYZ;
};

RX_SCENE_EXPORT WorldStreamObservation
MakeWorldStreamObservation(const Transform& transform, const WorldStreamObserver& observer);

// Gathers observer values without structurally mutating the ECS world.
RX_SCENE_EXPORT void GatherWorldStreamObservers(ecs::World& world,
                                                base::Vector<WorldStreamObservation>* observations);

}  // namespace rx::scene

#endif  // RX_SCENE_WORLD_STREAMING_ECS_H_
