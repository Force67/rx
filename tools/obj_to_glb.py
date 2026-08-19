#!/usr/bin/env python3
"""Wavefront OBJ -> self-contained binary glTF.

Written for the character look-dev bench: the best freely available human scans
ship as OBJ + loose JPEG maps, and rx loads glTF. This is deliberately a narrow
converter (positions, normals, uvs, one material, embedded JPEG/PNG textures) -
enough to get a scan onto the bench, not a general asset pipeline.

  tools/obj_to_glb.py in.obj out.glb --basecolor dif.jpg --normal norm.jpg \
      --scale 0.01 --recenter --yaw 180

--scale converts the source units to metres (scans are usually centimetres).
--recenter puts the model's feet at the origin and centres it in x/z.
--yaw rotates about y in degrees; rx's convention has a character facing -Z.
"""

import argparse
import base64
import json
import os
import struct
import sys


def parse_obj(path):
    positions, uvs, normals = [], [], []
    faces = []  # list of (vi, ti, ni) triples, already triangulated
    with open(path, "r", errors="replace") as f:
        for line in f:
            if not line or line[0] == "#":
                continue
            tag, _, rest = line.partition(" ")
            if tag == "v":
                positions.append(tuple(float(x) for x in rest.split()[:3]))
            elif tag == "vt":
                parts = rest.split()
                uvs.append((float(parts[0]), float(parts[1]) if len(parts) > 1 else 0.0))
            elif tag == "vn":
                normals.append(tuple(float(x) for x in rest.split()[:3]))
            elif tag == "f":
                corners = rest.split()
                parsed = []
                for corner in corners:
                    bits = corner.split("/")
                    vi = int(bits[0])
                    ti = int(bits[1]) if len(bits) > 1 and bits[1] else 0
                    ni = int(bits[2]) if len(bits) > 2 and bits[2] else 0
                    parsed.append((vi, ti, ni))
                # Fan-triangulate; scan exports are triangles or quads.
                for i in range(1, len(parsed) - 1):
                    faces.append((parsed[0], parsed[i], parsed[i + 1]))
    return positions, uvs, normals, faces


def resolve(index, count):
    # OBJ indices are 1-based and may be negative (relative to the end).
    if index > 0:
        return index - 1
    if index < 0:
        return count + index
    return -1


def build(positions, uvs, normals, faces):
    unique = {}
    out_pos, out_uv, out_nrm, indices = [], [], [], []
    for tri in faces:
        for corner in tri:
            key = corner
            got = unique.get(key)
            if got is None:
                vi = resolve(corner[0], len(positions))
                ti = resolve(corner[1], len(uvs))
                ni = resolve(corner[2], len(normals))
                got = len(out_pos)
                unique[key] = got
                out_pos.append(positions[vi])
                out_uv.append(uvs[ti] if 0 <= ti < len(uvs) else (0.0, 0.0))
                out_nrm.append(normals[ni] if 0 <= ni < len(normals) else (0.0, 1.0, 0.0))
            indices.append(got)
    return out_pos, out_uv, out_nrm, indices


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("obj")
    ap.add_argument("out")
    ap.add_argument("--basecolor")
    ap.add_argument("--normal")
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--recenter", action="store_true")
    ap.add_argument("--yaw", type=float, default=0.0)
    ap.add_argument("--roughness", type=float, default=0.45)
    ap.add_argument("--name", default="scan")
    args = ap.parse_args()

    positions, uvs, normals, faces = parse_obj(args.obj)
    if not faces:
        print("obj_to_glb: no faces", file=sys.stderr)
        return 1
    pos, uv, nrm, idx = build(positions, uvs, normals, faces)

    s = args.scale
    pos = [(p[0] * s, p[1] * s, p[2] * s) for p in pos]
    lo = [min(p[i] for p in pos) for i in range(3)]
    hi = [max(p[i] for p in pos) for i in range(3)]
    if args.recenter:
        cx = (lo[0] + hi[0]) * 0.5
        cz = (lo[2] + hi[2]) * 0.5
        pos = [(p[0] - cx, p[1] - lo[1], p[2] - cz) for p in pos]
        lo = [lo[0] - cx, 0.0, lo[2] - cz]
        hi = [hi[0] - cx, hi[1] - lo[1] if False else hi[1] - (lo[1] if False else 0.0), hi[2] - cz]
        lo = [min(p[i] for p in pos) for i in range(3)]
        hi = [max(p[i] for p in pos) for i in range(3)]

    # glTF wants tightly packed float32 / uint32.
    pos_bytes = b"".join(struct.pack("<3f", *p) for p in pos)
    nrm_bytes = b"".join(struct.pack("<3f", *n) for n in nrm)
    # OBJ's v origin is bottom-left, glTF's is top-left.
    uv_bytes = b"".join(struct.pack("<2f", u[0], 1.0 - u[1]) for u in uv)
    idx_bytes = struct.pack("<%dI" % len(idx), *idx)

    chunks = []
    views = []
    accessors = []
    offset = 0

    def add_view(data, target=None):
        nonlocal offset
        pad = (-len(data)) % 4
        view = {"buffer": 0, "byteOffset": offset, "byteLength": len(data)}
        if target:
            view["target"] = target
        views.append(view)
        chunks.append(data + b"\0" * pad)
        offset += len(data) + pad
        return len(views) - 1

    v_pos = add_view(pos_bytes, 34962)
    v_nrm = add_view(nrm_bytes, 34962)
    v_uv = add_view(uv_bytes, 34962)
    v_idx = add_view(idx_bytes, 34963)

    accessors.append({"bufferView": v_pos, "componentType": 5126, "count": len(pos),
                      "type": "VEC3", "min": lo, "max": hi})
    accessors.append({"bufferView": v_nrm, "componentType": 5126, "count": len(nrm),
                      "type": "VEC3"})
    accessors.append({"bufferView": v_uv, "componentType": 5126, "count": len(uv),
                      "type": "VEC2"})
    accessors.append({"bufferView": v_idx, "componentType": 5125, "count": len(idx),
                      "type": "SCALAR"})

    images, textures = [], []

    def add_image(path):
        if not path:
            return None
        ext = os.path.splitext(path)[1].lower()
        mime = "image/png" if ext == ".png" else "image/jpeg"
        with open(path, "rb") as f:
            data = f.read()
        view = add_view(data)
        images.append({"bufferView": view, "mimeType": mime})
        textures.append({"source": len(images) - 1, "sampler": 0})
        return len(textures) - 1

    tex_base = add_image(args.basecolor)
    tex_norm = add_image(args.normal)

    pbr = {"metallicFactor": 0.0, "roughnessFactor": args.roughness}
    if tex_base is not None:
        pbr["baseColorTexture"] = {"index": tex_base}
    material = {"name": args.name, "pbrMetallicRoughness": pbr}
    if tex_norm is not None:
        material["normalTexture"] = {"index": tex_norm}

    yaw = args.yaw * 3.141592653589793 / 180.0
    node = {"mesh": 0, "name": args.name,
            "rotation": [0.0, __import__("math").sin(yaw * 0.5), 0.0,
                         __import__("math").cos(yaw * 0.5)]}

    gltf = {
        "asset": {"version": "2.0", "generator": "rx obj_to_glb"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [node],
        "meshes": [{"name": args.name, "primitives": [
            {"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
             "indices": 3, "material": 0}]}],
        "materials": [material],
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"byteLength": offset}],
    }
    if images:
        gltf["images"] = images
        gltf["textures"] = textures
        gltf["samplers"] = [{"magFilter": 9729, "minFilter": 9987,
                             "wrapS": 10497, "wrapT": 10497}]

    bin_blob = b"".join(chunks)
    json_blob = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    json_blob += b" " * ((-len(json_blob)) % 4)

    total = 12 + 8 + len(json_blob) + 8 + len(bin_blob)
    with open(args.out, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(json_blob), 0x4E4F534A))
        f.write(json_blob)
        f.write(struct.pack("<II", len(bin_blob), 0x004E4942))
        f.write(bin_blob)
    print("%s: %d verts, %d tris, bounds %.3f..%.3f m (y)" %
          (args.out, len(pos), len(idx) // 3, lo[1], hi[1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
