#ifndef RX_SCENE_SCENE_HANDLERS_H_
#define RX_SCENE_SCENE_HANDLERS_H_

namespace rx::script {
class HandlerRegistry;
}

namespace rx::scene {

// Registers the scene/spatial script handlers into `reg`. Defined in
// scene_handlers.cc, which calls ecs::World and the scene components DIRECTLY --
// no gateway interface. The app calls this (alongside each other system's
// SetupXxxCommands) once at engine start. The command names use the game-facing
// "World." namespace, independent of the engine module they live in.
void SetupSceneCommands(script::HandlerRegistry& reg);

}  // namespace rx::scene

#endif  // RX_SCENE_SCENE_HANDLERS_H_
