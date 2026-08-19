#!/usr/bin/env python3
"""Fits a Realis-style measured residual for a character material.

    photograph - analytical render = residual

The residual is what the analytical model still gets wrong once it has been
fitted as far as it goes. This tool measures it under a set of calibrated OLAT
frames, projects it into the material's TEXTURE space, fits it to a directional
basis and writes the two maps the runtime reconstructs from
(asset::Material::HumanParams::residual_ambient / residual_directional).

Read the warning first
----------------------
This is the LAST step of the workflow, not a shortcut past the earlier ones. A
residual fitted over a badly fitted BRDF bakes the BRDF's errors into a texture
and freezes them: they stop responding to light, they stop responding to
material state, and they get worse the further the runtime drifts from the
capture. Do not run this until the BRDF, the geometry, the colour pipeline, the
lighting and the SSS are all stable (docs/CHARACTER_RENDERING.md).

Inputs
------
A manifest JSON:

    {
      "texture_size": 1024,
      "frames": [
        {"render": "olat_03_render.png",
         "photo":  "olat_03_photo.png",
         "uv":     "olat_03_uv.png",
         "light":  [0.0, -0.15, 1.0]},
        ...
      ]
    }

`render` and `photo` are the SAME framing under the SAME light, in the same
colour path (the look-dev bench's capture pass writes the render; the photo is
your calibrated OLAT plate, aligned to it). `uv` is the same framing captured
with debug view 23 (RX_DEBUG_VIEW=23), which writes the shaded texel's uv into
rg - that is what carries the screen-space difference back to a texel. `light`
is the light's TRAVEL direction in world space, the same convention as the
engine's sun.

Output
------
Two 8-bit maps, both SIGNED and stored biased (v*0.5+0.5):

    <prefix>_ambient.png       rgb the view-independent residual, a its coverage
    <prefix>_directional.png   rgb the fitted directional vector (tangent space)

The runtime evaluates `residual = amb * (1 + dot(dir, l_tangent)) * coverage`,
fades it out when the live material stops matching the captured one, and drops
it entirely below the hero tier.
"""

import argparse
import json
import math
import os
import struct
import sys
import zlib

import numpy as np


# --- minimal PNG io (no PIL dependency; the pipeline should not need one) -----

def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("%s: not a PNG (this tool reads PNG only)" % path)
    pos, idat, width, height, depth, ctype = 8, [], 0, 0, 8, 6
    while pos < len(data):
        length = struct.unpack_from(">I", data, pos)[0]
        kind = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, ctype = struct.unpack(">IIBB", chunk[:10])
        elif kind == b"IDAT":
            idat.append(chunk)
        elif kind == b"IEND":
            break
        pos += 12 + length
    if depth not in (8, 16) or ctype not in (2, 6):
        raise SystemExit("%s: need 8/16-bit RGB or RGBA, got depth %d type %d"
                         % (path, depth, ctype))
    channels = 3 if ctype == 2 else 4
    bpp = channels * (depth // 8)
    raw = zlib.decompress(b"".join(idat))
    stride = width * bpp
    out = np.zeros((height, stride), dtype=np.uint8)
    prev = np.zeros(stride, dtype=np.uint8)
    pos = 0
    for y in range(height):
        filt = raw[pos]
        pos += 1
        line = np.frombuffer(raw[pos:pos + stride], dtype=np.uint8).copy()
        pos += stride
        if filt == 1:
            for i in range(bpp, stride):
                line[i] = (int(line[i]) + int(line[i - bpp])) & 0xFF
        elif filt == 2:
            line = (line.astype(np.int32) + prev.astype(np.int32)).astype(np.uint8)
        elif filt == 3:
            for i in range(stride):
                left = int(line[i - bpp]) if i >= bpp else 0
                line[i] = (int(line[i]) + ((left + int(prev[i])) >> 1)) & 0xFF
        elif filt == 4:
            for i in range(stride):
                a = int(line[i - bpp]) if i >= bpp else 0
                b = int(prev[i])
                c = int(prev[i - bpp]) if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (int(line[i]) + pred) & 0xFF
        out[y] = line
        prev = line
    if depth == 8:
        img = out.reshape(height, width, channels).astype(np.float32) / 255.0
    else:
        big = out.reshape(height, width, channels, 2).astype(np.uint32)
        img = ((big[..., 0] << 8) | big[..., 1]).astype(np.float32) / 65535.0
    if channels == 3:
        img = np.concatenate([img, np.ones_like(img[..., :1])], axis=2)
    return img


def write_png(path, rgba):
    height, width, _ = rgba.shape
    body = (np.clip(rgba, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)
    raw = b"".join(b"\x00" + body[y].tobytes() for y in range(height))

    def chunk(kind, payload):
        return (struct.pack(">I", len(payload)) + kind + payload +
                struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("manifest")
    ap.add_argument("--out-prefix", required=True)
    ap.add_argument("--size", type=int, default=0,
                    help="texture size; defaults to the manifest's texture_size")
    ap.add_argument("--scale", type=float, default=1.0,
                    help="scene-linear range the stored +-1 covers")
    ap.add_argument("--srgb-input", action="store_true",
                    help="inputs are display-encoded; linearize on the way in")
    ap.add_argument("--dilate", type=int, default=4,
                    help="texels of edge dilation, so bilinear filtering at a uv "
                         "seam does not sample uncovered texels")
    args = ap.parse_args()

    manifest = json.load(open(args.manifest))
    frames = manifest.get("frames", [])
    if not frames:
        raise SystemExit("manifest has no frames")
    size = args.size or int(manifest.get("texture_size", 1024))
    root = os.path.dirname(os.path.abspath(args.manifest))

    # Per texel we solve, for luminance, the 4-coefficient least squares
    #     L(l) = b0 + b . l
    # and separately accumulate the mean per-channel residual. The runtime
    # basis is amb.rgb * (1 + dir . l), so dir = b / b0 and amb = mean rgb.
    # A rank-4 fit is all a handful of OLAT frames can support honestly; going
    # higher order fits the capture noise, and the residual is a correction,
    # not a second renderer.
    ata = np.zeros((size, size, 4, 4), dtype=np.float64)
    atb = np.zeros((size, size, 4), dtype=np.float64)
    rgb_sum = np.zeros((size, size, 3), dtype=np.float64)
    hits = np.zeros((size, size), dtype=np.float64)

    for frame in frames:
        def load(key):
            path = frame[key]
            if not os.path.isabs(path):
                path = os.path.join(root, path)
            img = read_png(path)
            return img

        render = load("render")
        photo = load("photo")
        uv_img = load("uv")
        if render.shape[:2] != photo.shape[:2] or render.shape[:2] != uv_img.shape[:2]:
            raise SystemExit("frame %s: render / photo / uv differ in size" % frame["render"])
        if args.srgb_input:
            render = srgb_to_linear(render)
            photo = srgb_to_linear(photo)

        light = np.array(frame["light"], dtype=np.float64)
        light /= max(np.linalg.norm(light), 1e-9)
        # The engine's convention is the direction the light TRAVELS; the basis
        # wants the direction toward the light.
        light = -light

        diff = (photo[..., :3] - render[..., :3]).astype(np.float64)
        lum = diff @ np.array([0.2126, 0.7152, 0.0722])

        # A uv of exactly (0,0) is what the background writes, so it is the one
        # value that cannot be trusted as a texel address.
        u = uv_img[..., 0]
        v = uv_img[..., 1]
        covered = (u + v) > 1e-4
        tx = np.clip((u * size).astype(np.int64), 0, size - 1)[covered]
        ty = np.clip((v * size).astype(np.int64), 0, size - 1)[covered]
        d = diff[covered]
        y = lum[covered]

        basis = np.array([1.0, light[0], light[1], light[2]], dtype=np.float64)
        outer = np.outer(basis, basis)
        flat = ty * size + tx
        np.add.at(ata.reshape(-1, 4, 4), flat, outer)
        np.add.at(atb.reshape(-1, 4), flat, np.outer(y, basis))
        np.add.at(rgb_sum.reshape(-1, 3), flat, d)
        np.add.at(hits.reshape(-1), flat, 1.0)

    covered = hits > 0.0
    print("coverage: %.1f%% of %dx%d texels, %d frames"
          % (100.0 * covered.mean(), size, size, len(frames)))
    if not covered.any():
        raise SystemExit("no texel was covered by any frame - check the uv captures")

    # Tikhonov: with only a handful of light directions the normal equations are
    # near-singular wherever the directions are close to coplanar, and an
    # unregularized solve there produces a huge direction vector that reads as a
    # violent, light-dependent artefact the moment the runtime leaves the
    # captured set.
    ridge = np.eye(4) * 1e-3
    solved = np.zeros((size, size, 4), dtype=np.float64)
    idx = np.argwhere(covered)
    for (ty, tx) in idx:
        solved[ty, tx] = np.linalg.solve(ata[ty, tx] + ridge, atb[ty, tx])

    amb = np.zeros((size, size, 3), dtype=np.float64)
    amb[covered] = rgb_sum[covered] / hits[covered][:, None]
    b0 = solved[..., 0]
    direction = np.zeros((size, size, 3), dtype=np.float64)
    # dir = b/b0 is only meaningful where the ambient term is large enough to
    # divide by. Where the residual passes through zero the ratio explodes, and
    # an exploded direction is a light-dependent artefact waiting to appear the
    # moment the runtime leaves the captured light set - so those texels take
    # the material's mean direction instead of their own noise.
    scale_ref = np.percentile(np.abs(b0[covered]), 60) if covered.any() else 0.0
    safe = covered & (np.abs(b0) > max(scale_ref * 0.25, 1e-4))
    direction[safe] = solved[..., 1:][safe] / b0[safe][:, None]
    if safe.any():
        mean_dir = direction[safe].mean(axis=0)
        fallback = covered & ~safe
        direction[fallback] = mean_dir
    # A directional term that more than doubles or cancels the ambient term is
    # not a correction any more; clamp so the runtime's max(1 + dot, 0) stays in
    # a range where the fit is meaningful.
    norm = np.linalg.norm(direction, axis=2, keepdims=True)
    direction = np.where(norm > 0.9, direction * (0.9 / np.maximum(norm, 1e-9)), direction)

    coverage = np.zeros((size, size), dtype=np.float64)
    coverage[covered] = np.minimum(hits[covered] / max(len(frames) * 0.5, 1.0), 1.0)

    # Dilate into the uncovered gutter so bilinear filtering at a uv seam does
    # not pull a zero texel into a covered one.
    for _ in range(max(args.dilate, 0)):
        grown = coverage.copy()
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            shifted = np.roll(coverage, (dy, dx), axis=(0, 1))
            take = (grown <= 0.0) & (shifted > 0.0)
            grown[take] = shifted[take]
            for arr in (amb, direction):
                rolled = np.roll(arr, (dy, dx), axis=(0, 1))
                arr[take] = rolled[take]
        coverage = grown

    amb_encoded = np.concatenate(
        [np.clip(amb / max(args.scale, 1e-6), -1.0, 1.0) * 0.5 + 0.5,
         coverage[..., None]], axis=2)
    dir_encoded = np.concatenate(
        [np.clip(direction, -1.0, 1.0) * 0.5 + 0.5,
         np.ones((size, size, 1))], axis=2)

    write_png(args.out_prefix + "_ambient.png", amb_encoded.astype(np.float32))
    write_png(args.out_prefix + "_directional.png", dir_encoded.astype(np.float32))
    residual_rms = float(np.sqrt(np.mean(np.sum(amb[covered] ** 2, axis=1))))
    print("residual rms %.5f (scene-linear); stored at +-%.3f" % (residual_rms, args.scale))
    print("wrote %s_ambient.png and %s_directional.png" % (args.out_prefix, args.out_prefix))
    print("set residual_weight > 0 and point the material at both maps to use them")
    return 0


if __name__ == "__main__":
    sys.exit(main())
