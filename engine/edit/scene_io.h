#ifndef RX_EDIT_SCENE_IO_H_
#define RX_EDIT_SCENE_IO_H_

#include <string>

#include "asset/asset_database.h"
#include "core/export.h"
#include "ecs/world.h"

// Text scene serialization (".rxscene"): a versioned, human-readable and
// git-diffable dump of every reflected component on every identity-bearing
// entity (one that has a Guid, Name or Transform). Components and props are
// written by their reflected names; entity-reference props are written as the
// target's Guid; a Renderable's AssetId is written as its source path when the
// asset system knows it (falling back to the raw hash). Unknown component/prop
// names on load are skipped with a warning rather than failing the load.
namespace rx::edit {

// Writes the scene to `file_path`. Assigns Guids to identity-bearing entities
// that lack one (so references resolve on reload). False + *error on I/O error.
RX_EDIT_EXPORT bool SaveScene(ecs::World& world, const std::string& file_path,
                              std::string* error = nullptr);

// Loads a scene into `world`, creating fresh entities, remapping Guid-based
// references and resolving Renderable paths through `db`. Existing entities in
// `world` are left untouched. False + *error on a parse/I/O error.
RX_EDIT_EXPORT bool LoadScene(ecs::World& world, asset::AssetDatabase& db,
                              const std::string& file_path, std::string* error = nullptr);

}  // namespace rx::edit

#endif  // RX_EDIT_SCENE_IO_H_
