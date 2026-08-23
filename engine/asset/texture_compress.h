#ifndef RX_ASSET_TEXTURE_COMPRESS_H_
#define RX_ASSET_TEXTURE_COMPRESS_H_

#include <string>
#include <string_view>

#include "asset/texture.h"
#include "core/export.h"

namespace rx::asset {

// Block compression for material textures, run at import.
//
// Why at import rather than offline: every texture rx actually loads today
// arrives as png/jpg, either loose next to a .rxscene or embedded in a glb, and
// a good part of them (the Pattern maps) do not exist on disk at all. An
// offline container would need every asset re-exported and would still leave
// the generated ones uncompressed. The encode is instead paid once per distinct
// image and cached on disk keyed by the source pixels, so a warm run costs a
// file read.
//
// The output always carries a baked mip chain. That is not a nicety:
// MaterialSystem cannot blit mips into a block-compressed image, and its
// residency system only streams textures that have both (block_dim == 4 and
// mip_count > 1), so a compressed texture without a chain would lose the memory
// twice over.

// What the texture is for. Which map a texture is bound as is knowable only in
// the loader that assigns the material slot, and it decides the block format,
// so it has to be passed in; `is_srgb` alone cannot tell a normal map from a
// roughness map.
enum class TextureRole : u8 {
  // Albedo/emissive. BC7 when the alpha channel is unused, BC3 when it is: BC3
  // gives the mask its own endpoints and indices, where BC7 mode 6 would make
  // one index set serve both rgb and alpha and drag colour across cutout edges.
  kColor,
  // Tangent-space normal. BC5, which stores xy at BC4 quality each and drops z.
  // ONLY for a map whose consumer reconstructs z: the raster shaders do that
  // behind MaterialSystem::kFlagNormalReconstructZ, which is set from the
  // uploaded format so the two cannot drift apart. A model-space normal, a
  // Bethesda specular mask in the normal's alpha, or a terrain splat layer
  // riding the normal slot all need three or four channels and must not use
  // this role.
  kNormalTangent,
  // Linear multi-channel data: glTF ORM, a lone roughness map, occlusion,
  // metallic. BC7, not BC4/BC5, deliberately - the shaders read roughness from
  // .g and metallic from .b, and a two-channel format returns 0 for the
  // channels it drops, which reads as a mirror-smooth surface rather than as a
  // broken texture.
  kData,
};

struct TextureCompressionOptions {
  // The device can sample BC1..BC7 (DeviceCaps::texture_compression_bc). Off
  // until a renderer says otherwise, so a run with no gpu at all (--validate,
  // tools, unit tests) keeps producing rgba8.
  bool supported = false;
  // Include tangent-space normal maps. OFF by default, and not because BC5 is
  // wrong - it is measurably the best compressed form for a normal map (49.5 dB
  // on xy against BC7's 43.4 in bc_encode_test, and 2-5 degrees closer to the
  // uncompressed normal than BC7 on the ambientCG maps island-game ships).
  // What is off is the pair of BC5 AND the z the shader has to rebuild from it.
  // That rebuild is exact only where the source texels are unit vectors that
  // stay well away from horizontal, and rx's own content is not:
  //   - Pattern's generated normal maps are the derivative of a step-edged
  //     height field, so at every pattern edge |xy| reaches 0.99 and z falls to
  //     0.14, where a one-level xy error swings the normal by degrees. Measured
  //     on runtime/scenes/showcase.rxscene: rmse 0.00925 against a 0.002 limit,
  //     from the normal maps alone. Colour and data maps on the same scene came
  //     in at 0.00148 and 0.00066.
  //   - jpeg-compressed normal maps (the usual CC0 texture-set format) have
  //     texels with |xy| > 1, which no reconstruction can represent; on
  //     island-game's set, simply rebuilding z with no compression at all
  //     already moves the normal by 0.4 to 6.4 degrees on average.
  // Turning this on roughly doubles the saving and is the right default for
  // content whose normal maps are authored to survive it. RX_TEX_COMPRESS_NORMALS=1.
  bool normals = false;
};

RX_ASSET_EXPORT void SetTextureCompression(TextureCompressionOptions options);
RX_ASSET_EXPORT TextureCompressionOptions TextureCompressionSettings();

// Compresses `texture` in place, building the mip chain first. `identity` names
// the source for the disk cache and should be stable across runs (a file path,
// or "<gltf path>#image3"); an empty identity still compresses but does not
// cache.
//
// False leaves the texture exactly as it was. That is a normal outcome, not an
// error: compression is off, the texture is already compressed, or its edges
// are not a usable multiple of four. It is deliberately NOT silent in
// aggregate - CompressionTotals().skipped counts every one of them, and the
// renderer prints the tally, because a change that quietly compressed nothing
// would otherwise look exactly like one that worked.
RX_ASSET_EXPORT bool CompressTexture(Texture* texture, TextureRole role,
                                     std::string_view identity);

struct TextureCompressionStats {
  u64 source_bytes = 0;      // rgba8 mip 0 handed in
  u64 compressed_bytes = 0;  // block bytes out, whole chain
  u32 compressed = 0;
  u32 skipped = 0;
  u32 cache_hits = 0;
  f64 encode_seconds = 0;  // wall time in the encoder, cache hits excluded
};
RX_ASSET_EXPORT TextureCompressionStats CompressionTotals();

}  // namespace rx::asset

#endif  // RX_ASSET_TEXTURE_COMPRESS_H_
