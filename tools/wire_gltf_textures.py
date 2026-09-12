#!/usr/bin/env python3
"""Embeds loose texture files into an existing .glb and points its first
material at them.

The best-known free head scans ship the mesh and the maps as separate files -
the glTF has a material with no textures at all - so loading one gives an
untextured blob. This wires them together into one self-contained .glb, and
optionally rescales the node (scan units are rarely metres, and the character
material's mean free paths are in metres, so scale is not cosmetic).

  tools/wire_gltf_textures.py in.glb out.glb --basecolor col.jpg \
      --normal nrm.jpg --scale 0.0257
"""

import argparse
import json
import math
import os
import struct
import sys


def read_glb(path):
    data = open(path, "rb").read()
    if data[:4] != b"glTF":
        raise SystemExit("%s: not a binary glTF" % path)
    off, gltf, blob = 12, None, b""
    while off < len(data):
        length, kind = struct.unpack_from("<II", data, off)
        off += 8
        chunk = data[off:off + length]
        if kind == 0x4E4F534A:
            gltf = json.loads(chunk)
        elif kind == 0x004E4942:
            blob = chunk
        off += length
    if gltf is None:
        raise SystemExit("%s: no JSON chunk" % path)
    return gltf, bytearray(blob)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("--basecolor")
    ap.add_argument("--normal")
    ap.add_argument("--metallic-roughness")
    ap.add_argument("--scale", type=float, default=0.0)
    ap.add_argument("--yaw", type=float, default=0.0)
    ap.add_argument("--roughness", type=float, default=0.45)
    ap.add_argument("--name")
    args = ap.parse_args()

    gltf, blob = read_glb(args.src)
    gltf.setdefault("bufferViews", [])
    gltf.setdefault("images", [])
    gltf.setdefault("textures", [])
    gltf.setdefault("samplers", [{"magFilter": 9729, "minFilter": 9987,
                                  "wrapS": 10497, "wrapT": 10497}])
    if not gltf["samplers"]:
        gltf["samplers"].append({"magFilter": 9729, "minFilter": 9987,
                                 "wrapS": 10497, "wrapT": 10497})

    def add_texture(path):
        if not path:
            return None
        payload = open(path, "rb").read()
        while len(blob) % 4:
            blob.append(0)
        offset = len(blob)
        blob.extend(payload)
        gltf["bufferViews"].append(
            {"buffer": 0, "byteOffset": offset, "byteLength": len(payload)})
        mime = "image/png" if os.path.splitext(path)[1].lower() == ".png" else "image/jpeg"
        gltf["images"].append({"bufferView": len(gltf["bufferViews"]) - 1, "mimeType": mime})
        gltf["textures"].append({"source": len(gltf["images"]) - 1, "sampler": 0})
        return len(gltf["textures"]) - 1

    base = add_texture(args.basecolor)
    normal = add_texture(args.normal)
    mr = add_texture(args.metallic_roughness)

    if not gltf.get("materials"):
        gltf["materials"] = [{}]
    material = gltf["materials"][0]
    if args.name:
        material["name"] = args.name
    pbr = material.setdefault("pbrMetallicRoughness", {})
    pbr["metallicFactor"] = 0.0
    pbr.setdefault("roughnessFactor", args.roughness)
    # A scan's authored baseColorFactor is usually a flat placeholder colour
    # that would tint the real map; the map is the measurement.
    pbr["baseColorFactor"] = [1.0, 1.0, 1.0, 1.0]
    if base is not None:
        pbr["baseColorTexture"] = {"index": base}
    if mr is not None:
        pbr["metallicRoughnessTexture"] = {"index": mr}
    if normal is not None:
        material["normalTexture"] = {"index": normal}

    # Only the node that actually carries the mesh; a scene file's camera and
    # lamp nodes are not part of the subject.
    if args.scale > 0.0 or args.yaw != 0.0:
        for node in gltf.get("nodes", []):
            if "mesh" not in node:
                continue
            if args.scale > 0.0:
                node["scale"] = [args.scale, args.scale, args.scale]
            if args.yaw != 0.0:
                half = args.yaw * math.pi / 360.0
                node["rotation"] = [0.0, math.sin(half), 0.0, math.cos(half)]
    # Some exporters leave the mesh out of the default scene (three.js's head
    # sits in an "AuxScene"); point the default scene at the mesh nodes.
    mesh_nodes = [i for i, n in enumerate(gltf.get("nodes", [])) if "mesh" in n]
    if mesh_nodes:
        gltf["scenes"] = [{"nodes": mesh_nodes}]
        gltf["scene"] = 0

    gltf["buffers"] = [{"byteLength": len(blob)}]
    json_blob = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    json_blob += b" " * ((-len(json_blob)) % 4)
    while len(blob) % 4:
        blob.append(0)
    total = 12 + 8 + len(json_blob) + 8 + len(blob)
    with open(args.out, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(json_blob), 0x4E4F534A))
        f.write(json_blob)
        f.write(struct.pack("<II", len(blob), 0x004E4942))
        f.write(bytes(blob))
    print("%s: %d textures embedded" % (args.out, len(gltf["textures"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
