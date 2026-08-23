#ifndef RX_ASSET_BC_ENCODE_H_
#define RX_ASSET_BC_ENCODE_H_

#include "core/export.h"
#include "core/types.h"

namespace rx::asset {

// Block-compression encoders for the 4x4 BCn formats rx uploads.
//
// Every Encode* takes one block worth of source texels and writes the packed
// block. Input blocks are always a full 4x4: a surface whose edge is not a
// multiple of 4 must replicate its edge texels into the padding, because the
// encoder fits endpoints over all 16 texels and garbage in the pad would drag
// them off the real data.
//
// The fit is "principal axis, then least-squares refit against the assigned
// indices", twice. That is the classic range-fit-and-refine; it lands within a
// fraction of a dB of an exhaustive search for a small fraction of the cost,
// which matters because this runs at load time (see texture_compress.h).

// 16 rgba8 texels, row major, 4 bytes each.
constexpr u32 kBlockTexels = 16;
constexpr u32 kBlockRgbaBytes = kBlockTexels * 4;

// BC1: 8 bytes. Colour only; the 3-colour (1-bit alpha) form is never emitted,
// so a decoder always reads four interpolated colours.
RX_ASSET_EXPORT void EncodeBc1Block(const u8* rgba, u8* out);

// BC3: 16 bytes. BC4 alpha block followed by the BC1 colour block. Alpha gets
// its own endpoints and its own indices, which is why masked (cutout) colour
// goes here rather than into BC7 mode 6, where one index set has to serve both
// rgb and alpha and the mask edge drags the colour with it.
RX_ASSET_EXPORT void EncodeBc3Block(const u8* rgba, u8* out);

// BC4: 8 bytes from 16 single-channel values.
RX_ASSET_EXPORT void EncodeBc4Block(const u8* values, u8* out);

// BC5: 16 bytes, two BC4 blocks. Source red lands in the first block, green in
// the second; blue and alpha are dropped, so only content whose consumer
// reconstructs the third channel may use this.
RX_ASSET_EXPORT void EncodeBc5Block(const u8* rgba, u8* out);

// BC7: 16 bytes, always mode 6 (one subset, rgba 7.7.7.7 endpoints with a
// p-bit each, 4-bit indices). Mode 6 alone because it is the only mode that
// needs no partition tables, and its 16-step index ramp over 8-bit-effective
// endpoints is what makes it beat BC1 on smooth albedo. Blocks that straddle
// two unrelated colour clusters are where the partitioned modes would win;
// adding mode 1 is the next quality step if one is ever needed.
RX_ASSET_EXPORT void EncodeBc7Block(const u8* rgba, u8* out);

// Decodes a BC7 block to 16 rgba8 texels. Only mode 6 is understood, which is
// every block EncodeBc7Block writes; anything else returns false rather than
// guessing, so a third-party BC7 texture degrades to "cannot read this" instead
// of to wrong pixels.
RX_ASSET_EXPORT bool DecodeBc7Block(const u8* block, u8* out_rgba);

}  // namespace rx::asset

#endif  // RX_ASSET_BC_ENCODE_H_
