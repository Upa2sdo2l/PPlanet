#!/usr/bin/env python3
"""Pack downloaded PBR texture sets into the 8 terrain layers of the planet.

Input: a folder with one subfolder per layer (names are case-insensitive):
    Sand  DryGrass  Grass  Forest  Jungle  Tundra  Snow  Rock
Each subfolder holds one texture set, e.g. from ambientCG, Poly Haven or Fab:
albedo/color/basecolor/diffuse, normal (GL or DX), roughness, height/displacement,
ambient occlusion. Missing maps get sensible defaults.

Output (per layer i, name N):
    T_Layer<i>_<N>_AH.png   RGB albedo (sRGB) + A height (0..1, stretched)
    T_Layer<i>_<N>_NRA.png  R normal X, G normal Y (DirectX), B roughness, A AO
plus layers.txt with the average colour of every layer (Hex sRGB) for the
material's far-distance colours.

Usage:
    pip install pillow numpy
    python pack_terrain_layers.py <input folder> <output folder> [--size 2048] [--flip-green Sand,Rock]

--flip-green forces a green-channel flip (OpenGL -> DirectX) for the listed
layers; by default a normal map whose file name contains "gl"/"opengl" is
flipped and any other is taken as DirectX.
"""
import argparse
import os
import re
import sys

try:
    import numpy as np
    from PIL import Image
except ImportError:
    sys.exit("Needs Pillow and numpy: pip install pillow numpy")

LAYERS = ["Sand", "DryGrass", "Grass", "Forest", "Jungle", "Tundra", "Snow", "Rock"]
KINDS = {
    "albedo":    ["albedo", "basecolor", "base_color", "basecolour", "diffuse", "diff", "color", "col"],
    "normal":    ["normal", "nor", "nrm"],
    "rough":     ["roughness", "rough"],
    "height":    ["height", "displacement", "disp", "bump"],
    "ao":        ["ambientocclusion", "ambient_occlusion", "occlusion", "ao"],
}
IMAGE_EXT = (".png", ".jpg", ".jpeg", ".tga", ".tif", ".tiff", ".exr", ".bmp")


def tokens(name):
    return [t for t in re.split(r"[^a-z0-9]+", name.lower()) if t]


def classify(path):
    toks = tokens(os.path.splitext(os.path.basename(path))[0])
    joined = "_".join(toks)
    for kind in ("normal", "rough", "height", "ao", "albedo"):   # albedo last: "color" is generic
        for key in KINDS[kind]:
            if key in toks or (len(key) > 4 and key in joined):
                return kind
    return None


def is_gl_normal(path):
    toks = tokens(os.path.basename(path))
    return any(t in ("gl", "opengl", "norgl", "normalgl") for t in toks) or "_gl" in path.lower()


def load(path, size, mode):
    img = Image.open(path)
    if img.mode in ("I;16", "I;16B", "I", "F"):
        arr = np.asarray(img).astype(np.float64)
        arr = (arr - arr.min()) / max(arr.max() - arr.min(), 1e-9)
        img = Image.fromarray((arr * 255).astype(np.uint8), "L")
    img = img.convert(mode).resize((size, size), Image.LANCZOS)
    return np.asarray(img).astype(np.float64) / 255.0


def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1 / 2.4) - 0.055)


def stretch(a):
    lo, hi = np.percentile(a, 1), np.percentile(a, 99)
    return np.clip((a - lo) / max(hi - lo, 1e-6), 0, 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--size", type=int, default=2048)
    ap.add_argument("--flip-green", default="", help="comma-separated layer names")
    args = ap.parse_args()

    flip = {n.strip().lower() for n in args.flip_green.split(",") if n.strip()}
    os.makedirs(args.output, exist_ok=True)
    subdirs = {d.lower(): os.path.join(args.input, d) for d in os.listdir(args.input)
               if os.path.isdir(os.path.join(args.input, d))}
    report = []

    for i, name in enumerate(LAYERS):
        folder = subdirs.get(name.lower())
        if not folder:
            print(f"[{i}] {name}: folder missing - skipped")
            continue
        found = {}
        for f in sorted(os.listdir(folder)):
            if not f.lower().endswith(IMAGE_EXT):
                continue
            kind = classify(f)
            if kind == "normal" and "normal" in found:
                # Prefer the DirectX variant when both are present.
                if is_gl_normal(found["normal"]) and not is_gl_normal(f):
                    found["normal"] = os.path.join(folder, f)
                continue
            if kind and kind not in found:
                found[kind] = os.path.join(folder, f)
        if "albedo" not in found:
            print(f"[{i}] {name}: no albedo found in {folder} - skipped")
            continue

        S = args.size
        albedo = load(found["albedo"], S, "RGB")
        height = stretch(load(found["height"], S, "L")) if "height" in found \
            else stretch(albedo @ np.array([0.299, 0.587, 0.114]))
        rough = load(found["rough"], S, "L") if "rough" in found else np.full((S, S), 0.8)
        ao = load(found["ao"], S, "L") if "ao" in found else np.ones((S, S))
        if "normal" in found:
            nrm = load(found["normal"], S, "RGB")
            gl = is_gl_normal(found["normal"]) or name.lower() in flip
            if gl:
                nrm[..., 1] = 1.0 - nrm[..., 1]
        else:
            nrm = np.zeros((S, S, 3)); nrm[..., 0] = 0.5; nrm[..., 1] = 0.5; nrm[..., 2] = 1.0
            gl = False

        ah = np.dstack([albedo, height])
        nra = np.dstack([nrm[..., 0], nrm[..., 1], rough, ao])
        base = os.path.join(args.output, f"T_Layer{i}_{name}")
        Image.fromarray((np.clip(ah, 0, 1) * 255 + 0.5).astype(np.uint8), "RGBA").save(base + "_AH.png")
        Image.fromarray((np.clip(nra, 0, 1) * 255 + 0.5).astype(np.uint8), "RGBA").save(base + "_NRA.png")

        mean_lin = srgb_to_linear(albedo).reshape(-1, 3).mean(0)
        hexcol = "".join(f"{int(round(v * 255)):02X}" for v in np.clip(linear_to_srgb(mean_lin), 0, 1))
        used = ", ".join(f"{k}={os.path.basename(v)}" for k, v in found.items())
        print(f"[{i}] {name}: average colour {hexcol}; normal {'OpenGL->DirectX' if gl else 'DirectX'}; {used}")
        report.append(f"{i}\t{name}\t{hexcol}")

    with open(os.path.join(args.output, "layers.txt"), "w") as fh:
        fh.write("index\tlayer\taverage colour (Hex sRGB)\n" + "\n".join(report) + "\n")
    print(f"\nDone: {args.output}. Average colours also in layers.txt.")


if __name__ == "__main__":
    main()
