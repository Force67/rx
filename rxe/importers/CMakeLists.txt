# Scene and material importers, one module each so a game links only the formats
# it reads and never a third-party parser it does not use (tinyusdz above all).
# They produce asset:: types (ImportedScene, Material); rx::asset itself parses
# nothing but its own formats.
add_subdirectory(gltf)
add_subdirectory(usd)
add_subdirectory(materialx)
add_subdirectory(blend)
