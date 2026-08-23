#ifndef RX_RUNTIME_SCENE_VALIDATE_H_
#define RX_RUNTIME_SCENE_VALIDATE_H_

#include <string>

// Structural validation of a .rxscene with no device and no window: the class
// of mistake that loads cleanly and then renders nothing, or renders something
// other than what the file says. Strict load already rejects misspelt component
// and prop NAMES; everything here is about values and about combinations the
// engine's own walks quietly skip.
//
// It lives beside scene_authoring.cc because it validates that file's
// components against that file's builder, and shares its tables so the two
// cannot disagree about what a scene may say.
namespace rx {

// Loads `path` and reports every structural problem it finds, as a
// compiler-style human report or (`json`) as one object on stdout. False when
// any error-level finding fired; warnings alone still return true.
bool ValidateSceneFile(const std::string& path, bool json);

}  // namespace rx

#endif  // RX_RUNTIME_SCENE_VALIDATE_H_
