#include "asset/image_file.h"

#include <cstring>
#include <format>

// The implementation lives in third_party/stb_impl.c (rx::stb_impl); this is
// only the declarations.
#include <stb_image.h>

namespace rx::asset {

std::string ImageFileProblem(const std::string& path) {
  if (path.empty()) return "is empty; there is nothing to load";
  int width = 0;
  int height = 0;
  int channels = 0;
  if (stbi_info(path.c_str(), &width, &height, &channels)) return {};
  // stb's reason separates "fopen failed" from a decode it refuses, which is
  // the difference between a mistyped path and a format this build cannot read.
  const char* reason = stbi_failure_reason();
  return std::format("does not read as an image ({}); the path is relative to the working "
                     "directory", reason ? reason : "unknown");
}

bool LoadImageFile(const std::string& path, bool srgb, AssetId id, Texture* out) {
  int width = 0;
  int height = 0;
  int channels = 0;
  stbi_uc* pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
  if (!pixels) return false;
  out->id = id;
  out->format = TextureFormat::kRgba8;
  out->width = static_cast<u32>(width);
  out->height = static_cast<u32>(height);
  out->mip_count = 1;
  out->array_layers = 1;
  out->is_srgb = srgb;
  out->data.resize(static_cast<size_t>(width) * height * 4);
  std::memcpy(out->data.data(), pixels, out->data.size());
  stbi_image_free(pixels);
  return true;
}

}  // namespace rx::asset
