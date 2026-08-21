#ifndef RX_ASSET_PROCEDURAL_TEXTURE_H_
#define RX_ASSET_PROCEDURAL_TEXTURE_H_

#include <string_view>

#include "asset/texture.h"
#include "core/export.h"

namespace rx::asset {

// Text-describable texture synthesis. An authoring tool that can write a scene
// file but cannot ship a PNG still needs surfaces that are not flat colour, so
// every pattern here is fully described by a handful of numbers.
//
// One scalar mask over the 0..1 uv square drives all three bakes below: the
// colour map mixes two colours by it, the normal map reads it as a height field
// and the roughness map ramps between two values. Sharing the mask is the whole
// point - a brick's relief and its albedo can never end up describing different
// bricks. Edges are smoothstepped rather than stepped so the derived normal map
// has a real slope to work with instead of a one-texel spike.
enum class PatternKind : u8 {
  kChecker,   // alternating cells, mask 1 on every other one
  kGrid,      // mask 1 inside the cell, 0 on the lines between them
  kBrick,     // mask 1 on the brick face, 0 in the mortar joint
  kGradient,  // linear ramp, mask = v
  kNoise,     // four octaves of tiling value noise
};

// Resolves a pattern name ("checker", "grid", "brick", "gradient", "noise") or
// returns false, so a caller can reject an unknown name instead of substituting
// one. Case sensitive, like the rest of the scene-file vocabulary.
RX_ASSET_EXPORT bool ParsePatternKind(std::string_view name, PatternKind* out);

struct PatternDesc {
  PatternKind kind = PatternKind::kChecker;
  u32 width = 256;
  u32 height = 256;
  // Cells across the uv square, per axis: [0] along u, [1] along v (bricks
  // across a course, and courses up the face). Whole numbers tile seamlessly;
  // anything else leaves a seam where uv wraps.
  //
  // Per axis rather than one number because the things these patterns are for
  // are not square. A facade is "five bays across and six floors up", and one
  // scalar can only say that on a cube: on any other box the cells come out
  // stretched by the face's aspect, which is a texel density nobody chose.
  f32 scale[2] = {4.0f, 4.0f};
  // Grid line / brick mortar width as a fraction of one cell. Unused by the
  // patterns that have no lines.
  f32 line_width = 0.08f;
  // Value-noise lattice seed; any change reshuffles the whole field.
  u32 seed = 0;
};

// The pattern mask at uv: 0 and 1 are the two ends every bake interpolates
// between, and the height field the normal bake differentiates. Wraps outside
// 0..1.
RX_ASSET_EXPORT f32 SamplePattern(const PatternDesc& desc, f32 u, f32 v);

// RGBA8 colour map, opaque, mixing `color_a` (mask 0) and `color_b` (mask 1).
// Both are LINEAR colours; `srgb` decides whether they are encoded on the way
// out and the texture tagged for the sRGB slots (base colour, emissive). Data
// maps must pass false or the GPU linearizes values that were never encoded.
RX_ASSET_EXPORT Texture MakePatternTexture(const PatternDesc& desc, const f32 color_a[3],
                                           const f32 color_b[3], bool srgb, AssetId id);

// RGBA8 tangent-space normal map (linear, OpenGL green-up: +y is +v) from the
// mask read as a height field, by central differences. `relief` is the depth the
// mask's full 0..1 range stands for, in uv units, so the result does not change
// with `width`/`height`: 0.02 reads as a shallow bevel, 0.1 as a deep groove.
RX_ASSET_EXPORT Texture MakePatternNormalMap(const PatternDesc& desc, f32 relief, AssetId id);

// RGBA8 glTF ORM map (linear) ramping roughness between the mask ends. Blue is
// left at 1 so a material's metallic factor passes through unscaled, matching
// the untextured case; red and alpha are unused by the shader.
RX_ASSET_EXPORT Texture MakePatternRoughnessMap(const PatternDesc& desc, f32 roughness_a,
                                                f32 roughness_b, AssetId id);

}  // namespace rx::asset

#endif  // RX_ASSET_PROCEDURAL_TEXTURE_H_
