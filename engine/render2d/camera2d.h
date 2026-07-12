#ifndef RX_RENDER2D_CAMERA2D_H_
#define RX_RENDER2D_CAMERA2D_H_

#include "core/math.h"
#include "render2d/types2d.h"

namespace rx::render2d {

// An orthographic 2D camera. World space is y-down (matching tile/screen
// conventions): +x right, +y down, one world unit == one pixel at zoom 1. The
// camera is fully CPU-side and GPU-agnostic - it just produces the view-proj
// the sprite pipeline consumes and the world<->screen mapping gameplay and
// picking need - so it is unit-testable without a device.
//
// The projection maps the visible world rect onto Vulkan clip space (x,y in
// [-1,1], y increasing downward, so +y world is down on screen). Depth is left
// to the caller: the sprite vertex shader writes each instance's own [0,1]
// depth, and the 2D pass runs with the depth test off (painter's order) by
// default, so the projection carries no z term.
class Camera2D {
 public:
  Camera2D() = default;

  // viewport is the render target size in pixels.
  void SetViewport(u32 width, u32 height) {
    viewport_ = {static_cast<f32>(width), static_cast<f32>(height)};
  }
  void SetCenter(Vec2 center) { center_ = center; }
  void MoveBy(Vec2 delta) { center_ += delta; }
  // Pixels per world unit. Clamped positive so the mapping stays invertible.
  void SetZoom(f32 zoom) { zoom_ = zoom > 1e-3f ? zoom : 1e-3f; }

  Vec2 center() const { return center_; }
  f32 zoom() const { return zoom_; }
  Vec2 viewport() const { return viewport_; }

  // World units currently visible.
  f32 visible_width() const { return viewport_.x / zoom_; }
  f32 visible_height() const { return viewport_.y / zoom_; }

  // World rect the camera currently frames, for tilemap / sprite culling.
  Rect VisibleRect() const {
    f32 hw = visible_width() * 0.5f;
    f32 hh = visible_height() * 0.5f;
    return {center_.x - hw, center_.y - hh, hw * 2.0f, hh * 2.0f};
  }

  // The column-major view-projection handed to the sprite pipeline. Maps world
  // (x,y) to clip (x,y) in [-1,1]; z is 0 (the shader supplies per-instance
  // depth) and w is 1.
  Mat4 ViewProj() const {
    f32 half_w = visible_width() * 0.5f;
    f32 half_h = visible_height() * 0.5f;
    Mat4 r;  // zero
    r.m[0] = 1.0f / half_w;
    r.m[5] = 1.0f / half_h;
    r.m[12] = -center_.x / half_w;
    r.m[13] = -center_.y / half_h;
    r.m[15] = 1.0f;
    return r;
  }

  Vec2 WorldToScreen(Vec2 world) const {
    return {(world.x - center_.x) * zoom_ + viewport_.x * 0.5f,
            (world.y - center_.y) * zoom_ + viewport_.y * 0.5f};
  }
  Vec2 ScreenToWorld(Vec2 screen) const {
    return {(screen.x - viewport_.x * 0.5f) / zoom_ + center_.x,
            (screen.y - viewport_.y * 0.5f) / zoom_ + center_.y};
  }

 private:
  Vec2 center_{};
  Vec2 viewport_{1280, 720};
  f32 zoom_ = 1.0f;
};

}  // namespace rx::render2d

#endif  // RX_RENDER2D_CAMERA2D_H_
