#ifndef RX_RENDER_EXR_WRITE_H_
#define RX_RENDER_EXR_WRITE_H_


#include "base/strings/xstring.h"
#include "foundation/build_config/export.h"
#include "foundation/build_config/types.h"

namespace rx::asset {

// Writes a 3-channel (RGB) 32-bit float, uncompressed, scanline OpenEXR. `rgb`
// is width*height*3 floats, row-major, top row first. Returns false on a file
// error. Minimal but spec-correct: enough for compositors (Nuke, Resolve, oiio,
// ffmpeg) to read the engine's linear-hdr captures as a production container.
RX_ASSET_EXPORT bool WriteExrRgbF32(const base::String& path, u32 width, u32 height, const f32* rgb);

}  // namespace rx::asset

#endif  // RX_RENDER_EXR_WRITE_H_
