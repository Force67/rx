#include "rxe/scene/fly_camera.h"

#include <math.h>

#include "foundation/math/scalar.h"

namespace rx::scene {

Vec3 FlyCamera::forward() const {
  return {::cosf(pitch_) * ::sinf(yaw_), ::sinf(pitch_), -::cosf(pitch_) * ::cosf(yaw_)};
}

void FlyCamera::Update(const ui::InputState& input, const FlyCameraInput& intent, bool allow_mouse,
                       bool allow_keyboard, f32 dt) {
  looking_ = allow_mouse && input.button(ui::MouseButton::kRight);

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
    yaw_ += intent.look_x * pad_sensitivity * dt;
    pitch_ -= intent.look_y * pad_sensitivity * dt * (invert_y ? -1.0f : 1.0f);
  }
  pitch_ = rx::Clamp(pitch_, -1.55f, 1.55f);

  if (!allow_keyboard) return;

  Vec3 fwd = forward();
  Vec3 right = Normalize(Cross(fwd, {0, 1, 0}));
  Vec3 move{};
  // Combined keyboard + left-stick planar movement.
  move += fwd * -intent.move_y;  // stick-up / W = forward
  move += right * intent.move_x;
  if (intent.rise) move += Vec3{0, 1, 0};
  if (intent.sink) move += Vec3{0, -1, 0};

  f32 length = ::sqrtf(Dot(move, move));
  if (length > 0) {
    f32 boost = intent.sprint ? 4.0f : 1.0f;
    position_ += move * (speed * boost * dt / length);
  }
}

}  // namespace rx::scene
