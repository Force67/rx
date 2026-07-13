#include "scene/camera.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "ecs/world.h"

namespace rx::scene {
namespace {

bool IsModeValid(ecs::World& world, ecs::Entity mode) {
  return world.IsAlive(mode) && world.Get<CameraMode>(mode) != nullptr;
}

f32 ApplyEasing(CameraEasing easing, f32 amount) {
  const f32 t = std::clamp(amount, 0.0f, 1.0f);
  switch (easing) {
    case CameraEasing::kLinear:
      return t;
    case CameraEasing::kSmoothStep:
      return t * t * (3.0f - 2.0f * t);
    case CameraEasing::kSmootherStep:
      return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
  }
  return t;
}

void CutToMode(CameraOutput& output, CameraStack& stack, ecs::Entity mode,
               const CameraMode& camera_mode) {
  output.view = camera_mode.view;
  output.view.orientation = Normalize(output.view.orientation);
  output.active_mode = mode;
  output.observed_discontinuity_revision = camera_mode.discontinuity_revision;
  ++output.history_revision;
  output.valid = true;
  stack.transition.active = false;
}

void BeginTransition(CameraOutput& output, CameraStack& stack, ecs::Entity mode,
                     const CameraMode& camera_mode, CameraTransitionSpec spec) {
  const bool had_output = output.valid;
  output.active_mode = mode;
  output.observed_discontinuity_revision = camera_mode.discontinuity_revision;
  output.valid = true;

  const bool compatible_projection =
      output.view.lens.projection == camera_mode.view.lens.projection;
  if (!std::isfinite(spec.duration) || spec.duration <= 0 || !had_output ||
      !compatible_projection) {
    CutToMode(output, stack, mode, camera_mode);
    return;
  }

  stack.transition.source = output.view;
  stack.transition.elapsed = 0;
  stack.transition.duration = spec.duration;
  stack.transition.easing = spec.easing;
  stack.transition.active = true;
}

bool PruneInvalidModes(ecs::World& world, CameraStack& stack, CameraOutput& output) {
  if (stack.entries.empty() || !IsModeValid(world, stack.entries[0].mode)) {
    stack.transition.active = false;
    output.active_mode = ecs::kInvalidEntity;
    output.valid = false;
    return false;
  }

  bool removed_top = false;
  for (size_t i = 1; i < stack.entries.size();) {
    if (IsModeValid(world, stack.entries[i].mode)) {
      ++i;
      continue;
    }
    if (i + 1 == stack.entries.size()) removed_top = true;
    stack.entries.erase(i);
  }

  if (removed_top) {
    const ecs::Entity mode = stack.entries.back().mode;
    CutToMode(output, stack, mode, *world.Get<CameraMode>(mode));
  }
  return true;
}

CameraStackResult GetStack(ecs::World& world, ecs::Entity output_entity, CameraStack** stack,
                           CameraOutput** output) {
  if (!world.IsAlive(output_entity)) return CameraStackResult::kInvalidOutput;
  *stack = world.Get<CameraStack>(output_entity);
  *output = world.Get<CameraOutput>(output_entity);
  if (!*stack || !*output) return CameraStackResult::kInvalidOutput;
  if (!PruneInvalidModes(world, **stack, **output)) return CameraStackResult::kInvalidBase;
  return CameraStackResult::kSuccess;
}

CameraStackResult RemoveActivation(ecs::World& world, CameraActivation activation,
                                   CameraTransitionSpec transition, bool require_top) {
  if (!activation) return CameraStackResult::kInvalidActivation;

  CameraStack* stack = nullptr;
  CameraOutput* output = nullptr;
  CameraStackResult result = GetStack(world, activation.output, &stack, &output);
  if (result != CameraStackResult::kSuccess) return result;

  size_t index = stack->entries.size();
  for (size_t i = 0; i < stack->entries.size(); ++i) {
    if (stack->entries[i].activation_id == activation.id) {
      index = i;
      break;
    }
  }
  if (index == stack->entries.size()) return CameraStackResult::kInvalidActivation;
  if (index == 0) return CameraStackResult::kBaseMode;

  const bool was_top = index + 1 == stack->entries.size();
  if (require_top && !was_top) return CameraStackResult::kNotTop;
  stack->entries.erase(index);
  if (!was_top) return CameraStackResult::kSuccess;

  const ecs::Entity mode = stack->entries.back().mode;
  BeginTransition(*output, *stack, mode, *world.Get<CameraMode>(mode), transition);
  return CameraStackResult::kSuccess;
}

}  // namespace

CameraStackResult InitializeCameraStack(ecs::World& world, ecs::Entity output,
                                        ecs::Entity base_mode) {
  if (!world.IsAlive(output)) return CameraStackResult::kInvalidOutput;
  CameraMode* mode = world.Get<CameraMode>(base_mode);
  if (!mode) return CameraStackResult::kInvalidBase;
  const CameraMode initial_mode = *mode;

  CameraStack stack;
  stack.entries.push_back({base_mode, 0});
  world.Add(output, std::move(stack));

  CameraOutput camera_output;
  camera_output.view = initial_mode.view;
  camera_output.view.orientation = Normalize(camera_output.view.orientation);
  camera_output.active_mode = base_mode;
  camera_output.observed_discontinuity_revision = initial_mode.discontinuity_revision;
  camera_output.valid = true;
  world.Add(output, std::move(camera_output));
  return CameraStackResult::kSuccess;
}

CameraPushResult PushCameraMode(ecs::World& world, ecs::Entity output_entity,
                                ecs::Entity mode_entity, CameraTransitionSpec transition) {
  CameraPushResult pushed;
  CameraMode* mode = world.Get<CameraMode>(mode_entity);
  if (!mode) {
    pushed.result = CameraStackResult::kInvalidMode;
    return pushed;
  }

  CameraStack* stack = nullptr;
  CameraOutput* output = nullptr;
  pushed.result = GetStack(world, output_entity, &stack, &output);
  if (pushed.result != CameraStackResult::kSuccess) return pushed;

  u64 activation_id = stack->next_activation_id++;
  if (activation_id == 0) activation_id = stack->next_activation_id++;
  stack->entries.push_back({mode_entity, activation_id});
  BeginTransition(*output, *stack, mode_entity, *mode, transition);
  pushed.activation = {output_entity, activation_id};
  return pushed;
}

CameraStackResult PopCameraMode(ecs::World& world, CameraActivation activation,
                                CameraTransitionSpec transition) {
  return RemoveActivation(world, activation, transition, true);
}

CameraStackResult ReleaseCameraMode(ecs::World& world, CameraActivation activation,
                                    CameraTransitionSpec transition) {
  return RemoveActivation(world, activation, transition, false);
}

bool IsCameraModeTop(ecs::World& world, CameraActivation activation) {
  if (!activation) return false;
  CameraStack* stack = world.Get<CameraStack>(activation.output);
  CameraOutput* output = world.Get<CameraOutput>(activation.output);
  if (!stack || !output || !PruneInvalidModes(world, *stack, *output) || stack->entries.empty())
    return false;
  return stack->entries.back().activation_id == activation.id;
}

void InvalidateCameraMode(ecs::World& world, ecs::Entity mode) {
  if (CameraMode* camera_mode = world.Get<CameraMode>(mode)) ++camera_mode->discontinuity_revision;
}

CameraView InterpolateCameraView(const CameraView& source, const CameraView& destination,
                                 f32 amount) {
  const f32 t = std::clamp(amount, 0.0f, 1.0f);
  CameraView view;
  view.position = Lerp(source.position, destination.position, t);
  view.orientation = Slerp(Normalize(source.orientation), Normalize(destination.orientation), t);
  view.lens = destination.lens;

  if (destination.lens.projection == CameraProjection::kPerspective) {
    const f32 source_scale = std::tan(source.lens.fov_y * 0.5f);
    const f32 destination_scale = std::tan(destination.lens.fov_y * 0.5f);
    view.lens.fov_y = 2.0f * std::atan(std::lerp(source_scale, destination_scale, t));
  } else {
    view.lens.ortho_height = std::lerp(source.lens.ortho_height, destination.lens.ortho_height, t);
    view.lens.ortho_near = std::lerp(source.lens.ortho_near, destination.lens.ortho_near, t);
    view.lens.ortho_far = std::lerp(source.lens.ortho_far, destination.lens.ortho_far, t);
  }
  return view;
}

void ResolveCameraStacks(ecs::World& world, f32 dt) {
  if (!std::isfinite(dt) || dt < 0) dt = 0;
  world.Each<CameraStack, CameraOutput>([&](ecs::Entity, CameraStack& stack, CameraOutput& output) {
    if (!PruneInvalidModes(world, stack, output)) return;

    const ecs::Entity mode_entity = stack.entries.back().mode;
    CameraMode* mode = world.Get<CameraMode>(mode_entity);
    if (!mode) return;

    if (output.active_mode != mode_entity) {
      CutToMode(output, stack, mode_entity, *mode);
      return;
    }
    if (output.observed_discontinuity_revision != mode->discontinuity_revision ||
        output.view.lens.projection != mode->view.lens.projection) {
      CutToMode(output, stack, mode_entity, *mode);
      return;
    }

    if (!stack.transition.active) {
      output.view = mode->view;
      output.view.orientation = Normalize(output.view.orientation);
      output.valid = true;
      return;
    }

    stack.transition.elapsed += dt;
    const f32 amount =
        stack.transition.duration > 0 ? stack.transition.elapsed / stack.transition.duration : 1.0f;
    output.view = InterpolateCameraView(stack.transition.source, mode->view,
                                        ApplyEasing(stack.transition.easing, amount));
    output.valid = true;
    if (amount >= 1.0f) {
      output.view = mode->view;
      output.view.orientation = Normalize(output.view.orientation);
      stack.transition.active = false;
    }
  });
}

}  // namespace rx::scene
