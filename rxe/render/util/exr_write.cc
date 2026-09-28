#include "base/containers/vector.h"
#include "base/strings/xstring.h"
#include "rxe/render/util/exr_write.h"

#include <stdio.h>
#include <string.h>

namespace rx::render {
namespace {

// The platform is little-endian x86_64 and EXR stores little-endian, so scalars
// serialize by raw copy.
void Push(base::Vector<u8>& b, const void* data, size_t n) {
  const u8* p = static_cast<const u8*>(data);
  b.insert(b.end(), p, p + n);
}
void PushI32(base::Vector<u8>& b, i32 v) { Push(b, &v, 4); }
void PushU64(base::Vector<u8>& b, u64 v) { Push(b, &v, 8); }
void PushF32(base::Vector<u8>& b, f32 v) { Push(b, &v, 4); }
void PushStr(base::Vector<u8>& b, const char* s) { Push(b, s, ::strlen(s) + 1); }  // with null

// One header attribute: name, type, value-size, value bytes.
void PushAttr(base::Vector<u8>& b, const char* name, const char* type, const base::Vector<u8>& value) {
  PushStr(b, name);
  PushStr(b, type);
  PushI32(b, static_cast<i32>(value.size()));
  Push(b, value.data(), value.size());
}

}  // namespace

bool WriteExrRgbF32(const base::String& path, u32 width, u32 height, const f32* rgb) {
  if (width == 0 || height == 0 || !rgb) return false;

  base::Vector<u8> out;
  PushI32(out, 0x01312f76);  // magic
  PushI32(out, 0x00000002);  // version 2, no flags (single-part scanline)

  // channels: alphabetical B, G, R, each 32-bit float, sampling 1x1.
  base::Vector<u8> channels;
  for (const char* name : {"B", "G", "R"}) {
    PushStr(channels, name);
    PushI32(channels, 2);  // FLOAT
    channels.push_back(0);  // pLinear
    channels.push_back(0);  // reserved[0]
    channels.push_back(0);  // reserved[1]
    channels.push_back(0);  // reserved[2]
    PushI32(channels, 1);   // xSampling
    PushI32(channels, 1);   // ySampling
  }
  channels.push_back(0);  // terminates the channel list
  PushAttr(out, "channels", "chlist", channels);

  base::Vector<u8> compression{0};  // NO_COMPRESSION
  PushAttr(out, "compression", "compression", compression);

  base::Vector<u8> data_window;
  PushI32(data_window, 0);
  PushI32(data_window, 0);
  PushI32(data_window, static_cast<i32>(width) - 1);
  PushI32(data_window, static_cast<i32>(height) - 1);
  PushAttr(out, "dataWindow", "box2i", data_window);
  PushAttr(out, "displayWindow", "box2i", data_window);

  base::Vector<u8> line_order{0};  // INCREASING_Y
  PushAttr(out, "lineOrder", "lineOrder", line_order);

  base::Vector<u8> aspect;
  PushF32(aspect, 1.0f);
  PushAttr(out, "pixelAspectRatio", "float", aspect);

  base::Vector<u8> window_center;
  PushF32(window_center, 0.0f);
  PushF32(window_center, 0.0f);
  PushAttr(out, "screenWindowCenter", "v2f", window_center);

  base::Vector<u8> window_width;
  PushF32(window_width, 1.0f);
  PushAttr(out, "screenWindowWidth", "float", window_width);

  out.push_back(0);  // end of header

  // Scanline offset table, then the scanline blocks (y, byte count, planar BGR).
  const u64 line_pixel_bytes = static_cast<u64>(width) * 3u * 4u;
  const u64 line_block_bytes = 8u + line_pixel_bytes;  // i32 y + i32 size + data
  const u64 data_start = out.size() + static_cast<u64>(height) * 8u;
  for (u32 y = 0; y < height; ++y) PushU64(out, data_start + static_cast<u64>(y) * line_block_bytes);

  for (u32 y = 0; y < height; ++y) {
    PushI32(out, static_cast<i32>(y));
    PushI32(out, static_cast<i32>(line_pixel_bytes));
    const f32* row = rgb + static_cast<size_t>(y) * width * 3u;
    for (u32 c = 2; c != static_cast<u32>(-1); --c) {  // B(2), G(1), R(0) order
      for (u32 x = 0; x < width; ++x) PushF32(out, row[x * 3u + c]);
    }
  }

  FILE* f = ::fopen(path.c_str(), "wb");
  if (!f) return false;
  size_t written = ::fwrite(out.data(), 1, out.size(), f);
  ::fclose(f);
  return written == out.size();
}

}  // namespace rx::render
