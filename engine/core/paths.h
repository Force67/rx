#ifndef RX_CORE_PATHS_H_
#define RX_CORE_PATHS_H_

#include <filesystem>

#include "core/export.h"

namespace rx {

// Directory the running executable sits in, or the working directory when the
// platform will not say. This is where a shipped build finds the files it
// carries next to itself: the working directory is whatever the shell or the
// launcher happened to be in, and the build-tree paths baked in at compile time
// exist only on the machine that did the build.
RX_CORE_EXPORT std::filesystem::path ExecutableDirectory();

}  // namespace rx

#endif  // RX_CORE_PATHS_H_
