#!/usr/bin/env python3
"""Turn a detailed scooter .glb into a PSX-sized, flat-coloured mesh.

Usage: make_scooter.py <in.glb> <out_header.h> [preview.png] [--faces N] [--flip]
                       [--credit "attribution line"]

Every part keeps its material's colour (a textured material is averaged), each
part is decimated to a share of the triangle budget proportional to its surface
area, and the result is scaled so the scooter is SCOOTER_LENGTH_M long, centred,
wheels on y = 0, front along +z. The header holds Q12 vertices with their
part's colour and a smoothed normal, so the game can light it as it turns.
"""
import sys

import fast_simplification
import numpy as np
import trimesh

SCOOTER_LENGTH_M = 1.7
LIGHT = np.array([0.35, 0.85, -0.4])


def part_colour(geom):
    mat = getattr(geom.visual, "material", None)
    if mat is None:
        return np.array([150, 150, 150])
    tex = getattr(mat, "baseColorTexture", None)
    if tex is not None:
        img = np.asarray(tex.convert("RGB"), dtype=np.float32)
        col = img.reshape(-1, 3).mean(axis=0)
        fac = getattr(mat, "baseColorFactor", None)
        if fac is not None:
            col = col * np.asarray(fac[:3], dtype=np.float32) / (255.0 if np.max(fac) > 1 else 1.0)
        return col
    fac = getattr(mat, "baseColorFactor", None)
    if fac is None:
        return np.array([150, 150, 150])
    fac = np.asarray(fac[:3], dtype=np.float32)
    return fac if fac.max() > 1.0 else fac * 255.0


def main():
    argv = sys.argv[1:]
    budget = 900
    credit = "unattributed model"
    if "--credit" in argv:
        i = argv.index("--credit")
        credit = argv[i + 1]
        del argv[i:i + 2]
    flip = "--flip" in argv  # the model's front faces its long axis' negative end
    if flip:
        argv.remove("--flip")
    if "--faces" in argv:
        i = argv.index("--faces")
        budget = int(argv[i + 1])
        del argv[i:i + 2]
    args = argv
    src, out = args[0], args[1]
    preview = args[2] if len(args) > 2 else None

    scene = trimesh.load(src, force="scene")
    parts = []
    for name in scene.graph.nodes_geometry:
        tf, gname = scene.graph[name]
        geom = scene.geometry[gname]
        m = geom.copy()
        m.apply_transform(tf)
        # glTF splits vertices at UV seams; welded, the simplifier can collapse them.
        m.merge_vertices(merge_tex=True, merge_norm=True)
        ext = m.bounds[1] - m.bounds[0]
        # A display base: a big flat disc under the model.
        if ext[1] < 0.02 * max(ext[0], ext[2]) and max(ext[0], ext[2]) > 1.0:
            print("dropped flat base", gname)
            continue
        parts.append((m, part_colour(geom)))

    areas = np.array([p[0].area for p in parts])
    allv = np.vstack([p[0].vertices for p in parts])
    lo, hi = allv.min(axis=0), allv.max(axis=0)
    size = hi - lo
    print("bounds", lo.round(2), hi.round(2), "size", size.round(2))
    long_axis = int(np.argmax(size))
    scale = SCOOTER_LENGTH_M / size[long_axis]

    verts, faces, cols = [], [], []
    for (m, col), a in zip(parts, areas):
        # Bolts and trim: a few pixels at PSX resolution, so not worth triangles.
        if a < areas.sum() * 0.0005:
            continue
        target = max(4, int(round(budget * a / areas.sum())))
        v, f = np.asarray(m.vertices, dtype=np.float32), np.asarray(m.faces, dtype=np.int32)
        # Repeated passes: a single pass stops early on awkward topology.
        for _ in range(6):
            if len(f) <= target * 1.2:
                break
            v, f = fast_simplification.simplify(v, f, target_reduction=min(1.0 - target / len(f), 0.99))
        if len(f) == 0:
            continue
        base = sum(len(x) for x in verts)
        verts.append(v)
        faces.append(f + base)
        cols += [col] * len(f)
    V = np.vstack(verts).astype(np.float64)
    F = np.vstack(faces)
    C = np.array(cols)

    # Centre on x/z, wheels on the ground, metres.
    V = (V - np.array([(lo[0] + hi[0]) / 2, lo[1], (lo[2] + hi[2]) / 2])) * scale
    # Long axis along z, up along y (glTF is already y-up).
    if long_axis == 0:
        V = V[:, [2, 1, 0]]
    if flip:  # half a turn about y, so the front ends up at +z
        V[:, 0] = -V[:, 0]
        V[:, 2] = -V[:, 2]

    # Per-vertex colour (each vertex belongs to one part) and smoothed normal.
    VC = np.zeros((len(V), 3))
    VC[F[:, 0]] = C
    VC[F[:, 1]] = C
    VC[F[:, 2]] = C
    fn = np.cross(V[F[:, 1]] - V[F[:, 0]], V[F[:, 2]] - V[F[:, 0]])
    VN = np.zeros_like(V)
    for k in range(3):
        np.add.at(VN, F[:, k], fn)
    VN /= np.linalg.norm(VN, axis=1, keepdims=True) + 1e-12

    print("vertices", len(V), "triangles", len(F))
    # Game space: Q12 metres, y grows downwards.
    G = np.round(V * 4096).astype(int)
    G[:, 1] = -G[:, 1]
    N = np.round(VN * 4096).astype(int)
    N[:, 1] = -N[:, 1]
    VCq = np.clip(VC * 0.5, 0, 255).astype(int)  # PSX colour: 128 = neutral
    with open(out, "w") as h:
        h.write("/* GENERATED by pc_port/tools/revenge/make_scooter.py -- do not edit.\n")
        h.write(" * %s */\n" % credit)
        h.write("#define SCOOTER_VERT_COUNT %d\n#define SCOOTER_TRI_COUNT %d\n" % (len(G), len(F)))
        h.write("typedef struct { s16 x, y, z, nx, ny, nz; u8 r, g, b, pad; } s_ScooterVert;\n")
        h.write("static const s_ScooterVert SCOOTER_VERTS[SCOOTER_VERT_COUNT] = {\n")
        for (x, y, z), (nx, ny, nz), (r, g, b) in zip(G, N, VCq):
            h.write("    { %d, %d, %d, %d, %d, %d, %d, %d, %d, 0 },\n" % (x, y, z, nx, ny, nz, r, g, b))
        h.write("};\nstatic const u16 SCOOTER_TRIS[SCOOTER_TRI_COUNT][3] = {\n")
        for a, b, c in F:
            h.write("    { %d, %d, %d },\n" % (a, b, c))
        h.write("};\n")
    RGB = np.clip(C * 0.5, 0, 255).astype(int)

    if preview:
        from PIL import Image, ImageDraw
        W = 1200
        img = Image.new("RGB", (W, 400), (40, 40, 48))
        d = ImageDraw.Draw(img)
        views = [("side", lambda p: (p[:, 2], -p[:, 1], p[:, 0])),
                 ("front", lambda p: (p[:, 0], -p[:, 1], p[:, 2])),
                 ("3/4", lambda p: ((p[:, 0] + p[:, 2]) * 0.7, -p[:, 1] + (p[:, 0] - p[:, 2]) * 0.2, p[:, 0] - p[:, 2]))]
        for vi, (name, proj) in enumerate(views):
            x, y, z = proj(V)
            s = 150
            cx, cy = 200 + vi * 400, 330
            order = np.argsort([-z[f].mean() for f in F])
            for t in order:
                pts = [(cx + x[i] * s, cy + y[i] * s) for i in F[t]]
                d.polygon(pts, fill=tuple(int(c) for c in np.clip(RGB[t] * 2, 0, 255)))
            d.text((cx - 20, 10), name, fill=(220, 220, 220))
        img.save(preview)


if __name__ == "__main__":
    main()
