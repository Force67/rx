// The one definition of the vendored stb implementations in the whole build.
// rx_asset decodes source-format images here, rx_render writes captures, and
// tinyusdz (which vendors the same v2.30 headers) is configured to call these
// rather than emit a second copy - see third_party/CMakeLists.txt.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
