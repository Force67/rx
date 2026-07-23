#include "render2d/camera2d.h"
#include "render2d/iso.h"
#include "render2d/sprite_renderer.h"
#include "render2d/tile_map.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace {

using namespace rx::render2d;

int failures = 0;

void Check(bool condition, const char* message) {
  if (condition) return;
  std::fprintf(stderr, "render2d_test: FAIL: %s\n", message);
  ++failures;
}

void Near(float actual, float expected, const char* message, float epsilon = 1e-4f) {
  if (std::abs(actual - expected) <= epsilon) return;
  std::fprintf(stderr, "render2d_test: FAIL: %s (got %.6f, expected %.6f)\n", message, actual,
               expected);
  ++failures;
}

void TestCameraRoundTrip() {
  Camera2D camera;
  camera.SetViewport(800, 600);
  camera.SetCenter({120.0f, -45.0f});
  camera.SetZoom(2.5f);

  const Vec2 world{181.0f, 17.0f};
  const Vec2 screen = camera.WorldToScreen(world);
  const Vec2 round_trip = camera.ScreenToWorld(screen);
  Near(round_trip.x, world.x, "camera world-to-screen x round-trip");
  Near(round_trip.y, world.y, "camera world-to-screen y round-trip");
  Near(camera.visible_width(), 320.0f, "camera visible width");
  Near(camera.visible_height(), 240.0f, "camera visible height");

  const Rect visible = camera.VisibleRect();
  Check(visible.Contains(camera.center()), "visible rect contains camera center");
}

void TestCameraRejectsDegenerateInputs() {
  Camera2D camera;
  camera.SetViewport(0, 0);
  camera.SetZoom(std::numeric_limits<float>::infinity());
  const rx::Mat4 view_proj = camera.ViewProj();
  Check(camera.viewport().x == 1.0f && camera.viewport().y == 1.0f,
        "zero viewport is kept invertible");
  Check(std::isfinite(view_proj.m[0]) && std::isfinite(view_proj.m[5]),
        "degenerate camera inputs produce a finite matrix");
}

void TestIsoPicking() {
  IsoGrid grid{.tile_w = 96.0f, .tile_h = 48.0f, .origin = {13.0f, -7.0f}};
  const Vec2i cells[] = {{0, 0}, {4, 2}, {-3, 5}};
  for (Vec2i cell : cells) {
    Check(grid.WorldToCell(grid.CellCenterWorld(cell)) == cell,
          "isometric cell center picks its source cell");
  }

  const Vec2 fractional{2.25f, -1.5f};
  const Vec2 world = grid.CellToWorld(fractional.x, fractional.y);
  const Vec2 round_trip = grid.WorldToCellF(world);
  Near(round_trip.x, fractional.x, "isometric fractional x round-trip");
  Near(round_trip.y, fractional.y, "isometric fractional y round-trip");
}

void TestTilesetValidation() {
  Tileset tileset{.tile_w = 16, .tile_h = 8, .columns = 4, .rows = 2,
                  .atlas_w = 64, .atlas_h = 16};
  const Rect uv = tileset.TileUv(6);
  Near(uv.x, 0.5f, "tileset uv x");
  Near(uv.y, 0.5f, "tileset uv y");
  Near(uv.w, 0.25f, "tileset uv width");
  Near(uv.h, 0.5f, "tileset uv height");
  Check(tileset.HasTile(7), "last tile id is valid");
  Check(!tileset.HasTile(8), "out-of-range tile id is invalid");
  Check(tileset.TileUv(8).w == 0.0f, "out-of-range tile id is rejected");

  tileset.columns = 0;
  Check(tileset.TileUv(0).w == 0.0f, "zero-column tileset is rejected");
}

void TestTileMapQueries() {
  TileMap map;
  map.tile_size = 16.0f;
  TileLayer& layer = map.AddLayer(3, 2, 0.5f, 1.0f, true);
  layer.Set(1, 1, 7);

  Check(map.WorldToTile({-0.1f, -0.1f}) == Vec2i{-1, -1},
        "negative world positions floor to negative tiles");
  Check(map.IsSolidWorld({17.0f, 17.0f}), "collision layer marks a world point solid");
  Check(!map.IsSolidTile(0, 0), "empty collision tile stays non-solid");
  Near(map.WorldBounds().w, 48.0f, "tilemap world width");
  Near(map.WorldBounds().h, 32.0f, "tilemap world height");

  map.tile_size = 0.0f;
  layer.Set(0, 0, 2);
  Check(map.WorldToTile({100.0f, 100.0f}) == Vec2i{}, "zero tile size is rejected");
  Check(!map.IsSolidWorld({100.0f, 100.0f}), "invalid tile size is never solid");
  Check(map.WorldBounds().w == 0.0f, "invalid tile size has empty bounds");
  map.tile_size = 1.0f;
  Check(!map.IsSolidWorld({std::numeric_limits<float>::infinity(), 0.0f}),
        "non-finite world position is never solid");
}

void TestRendererStartsInert() {
  SpriteRenderer renderer;
  Check(!renderer.ready(), "sprite renderer starts inert");
}

}  // namespace

int main() {
  TestCameraRoundTrip();
  TestCameraRejectsDegenerateInputs();
  TestIsoPicking();
  TestTilesetValidation();
  TestTileMapQueries();
  TestRendererStartsInert();

  if (failures != 0) {
    std::fprintf(stderr, "render2d_test: %d failure(s)\n", failures);
    return 1;
  }
  std::puts("render2d_test: all tests passed");
  return 0;
}
