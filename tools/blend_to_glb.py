"""Blender-side conversion used by rx::asset::ConvertBlendScene.

The source file is already open when this script runs. It exports render-visible
meshes plus their armatures to a game-oriented GLB. Current character-shaping
keys are baked into a new neutral basis; only runtime deformation/corrective
keys are retained, preventing authoring libraries with hundreds of inactive
body variants from producing multi-gigabyte runtime assets.
"""

import argparse
import os
import sys

import bpy


# These are compatibility filters for immutable, source-authored names. Exported
# runtime systems and user-facing diagnostics use chest/glutes terminology.
SOURCE_RUNTIME_SHAPE_TOKENS = (
    "breast", "pectoral", "abdomen", "stomach", "glute", "thigh", "calf",
    "flexquad", "flexhamstring", "flexbiceps", "flextriceps",
)


def parse_args():
    args = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    parser.add_argument("--manifest", required=True)
    return parser.parse_args(args)


def keep_shape(name):
    lowered = name.lower()
    return any(token in lowered for token in SOURCE_RUNTIME_SHAPE_TOKENS)


def bake_and_prune_shapes(obj):
    keys = obj.data.shape_keys
    if not keys or len(keys.key_blocks) <= 1:
        return []

    blocks = list(keys.key_blocks)
    vertex_count = len(obj.data.vertices)
    basis = blocks[0]
    current = [basis.data[i].co.copy() for i in range(vertex_count)]
    for block in blocks[1:]:
        if abs(block.value) < 1.0e-8:
            continue
        relative = block.relative_key or basis
        value = block.value
        for i in range(vertex_count):
            current[i] += (block.data[i].co - relative.data[i].co) * value

    retained = []
    for block in blocks[1:]:
        if not keep_shape(block.name):
            continue
        relative = block.relative_key or basis
        deltas = [block.data[i].co - relative.data[i].co for i in range(vertex_count)]
        retained.append((block.name, deltas))

    obj.shape_key_clear()
    for i, coordinate in enumerate(current):
        obj.data.vertices[i].co = coordinate
    obj.shape_key_add(name="Basis", from_mix=False)
    for name, deltas in retained:
        target = obj.shape_key_add(name=name, from_mix=False)
        target.value = 0.0
        for i, delta in enumerate(deltas):
            target.data[i].co = current[i] + delta
    return [name for name, _ in retained]


def main():
    options = parse_args()
    visible_meshes = [
        obj for obj in bpy.context.scene.objects
        if obj.type == "MESH" and not obj.hide_render and obj.visible_get()
    ]
    if not visible_meshes:
        raise RuntimeError("blend contains no render-visible meshes")

    # Work on copies so linked data and the open authoring scene stay untouched.
    bpy.ops.object.select_all(action="DESELECT")
    export_objects = []
    armatures = set()
    manifest_rows = []
    for source in visible_meshes:
        duplicate = source.copy()
        duplicate.data = source.data.copy()
        bpy.context.scene.collection.objects.link(duplicate)
        retained = bake_and_prune_shapes(duplicate)
        duplicate.select_set(True)
        export_objects.append(duplicate)
        manifest_rows.append((duplicate.name, len(duplicate.data.vertices), retained))
        for modifier in duplicate.modifiers:
            if modifier.type == "ARMATURE" and modifier.object:
                armatures.add(modifier.object)

    for armature in armatures:
        armature.select_set(True)
        export_objects.append(armature)

    bpy.context.view_layer.objects.active = export_objects[0]
    os.makedirs(os.path.dirname(os.path.abspath(options.output)), exist_ok=True)
    result = bpy.ops.export_scene.gltf(
        filepath=options.output,
        export_format="GLB",
        use_selection=True,
        export_materials="PLACEHOLDER",
        export_texcoords=True,
        export_normals=True,
        export_tangents=False,
        export_animations=False,
        export_skins=True,
        export_def_bones=True,
        export_influence_nb=4,
        export_all_influences=False,
        export_morph=True,
        export_morph_normal=False,
        export_morph_tangent=False,
        export_cameras=False,
        export_lights=False,
        export_yup=True,
        export_apply=False,
        export_extras=True,
    )
    if "FINISHED" not in result:
        raise RuntimeError("Blender glTF export did not finish")

    with open(options.manifest, "w", encoding="utf-8") as stream:
        stream.write("rx-blend-import-v1\n")
        stream.write("meshes=%d\n" % len(manifest_rows))
        stream.write("armatures=%d\n" % len(armatures))
        for name, vertices, retained in manifest_rows:
            stream.write("mesh=%s\tvertices=%d\tmorphs=%s\n" %
                         (name.replace("\t", " "), vertices, ",".join(retained)))

    print("RX_BLEND_EXPORT meshes=%d armatures=%d output=%s" %
          (len(manifest_rows), len(armatures), options.output))


if __name__ == "__main__":
    main()
