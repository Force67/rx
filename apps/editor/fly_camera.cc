#include "core/scalar.h"
#include "fly_camera.h"

#include <math.h>

namespace rx {

Vec3 FlyCamera::forward() const {
  return {::cosf(pitch_) * ::sinf(yaw_), ::sinf(pitch_), -::cosf(pitch_) * ::cosf(yaw_)};
}

void FlyCamera::Update(const InputState& input, const ActionState& actions, bool allow_mouse,
                       bool allow_keyboard, f32 dt) {
  looking_ = allow_mouse && input.button(MouseButton::kRight);

  if (looking_) {
    yaw_ += input.mouse_dx * sensitivity;
    pitch_ -= input.mouse_dy * sensitivity;
    if (input.wheel != 0) {
      speed *= ::powf(1.2f, input.wheel);
      speed = rx::Clamp(speed, 0.1f, 200.0f);
    }
  }
  // Right-stick look (rate based), no button needed.
  if (allow_keyboard) {
    yaw_ += actions.axis(Axis::kLookX) * pad_sensitivity * dt;
    pitch_ -= actions.axis(Axis::kLookY) * pad_sensitivity * dt * (invert_y ? -1.0f : 1.0f);
  }
  pitch_ = rx::Clamp(pitch_, -1.55f, 1.55f);

  if (!allow_keyboard) return;

  Vec3 fwd = forward();
  Vec3 right = Normalize(Cross(fwd, {0, 1, 0}));
  Vec3 move{};
  // Combined keyboard + left-stick planar movement.
  move += fwd * -actions.axis(Axis::kMoveY);  // stick-up / W = forward
  move += right * actions.axis(Axis::kMoveX);
  if (actions.down(Action::kCamUp) || actions.down(Action::kJump)) move += Vec3{0, 1, 0};
  if (actions.down(Action::kCamDown) || actions.down(Action::kSneak)) move += Vec3{0, -1, 0};

  f32 length = ::sqrtf(Dot(move, move));
  if (length > 0) {
    f32 boost = actions.down(Action::kSprint) ? 4.0f : 1.0f;
    position_ += move * (speed * boost * dt / length);
  }
}

}  // namespace rx
