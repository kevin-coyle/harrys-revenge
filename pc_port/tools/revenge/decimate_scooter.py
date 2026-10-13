"""Blender pass for make_scooter.py: decimate every part of a detailed .glb.

Run: Blender -b --python decimate_scooter.py -- <in.glb> <out.glb> <triangle budget>

Each mesh gets a share of the budget proportional to its surface area (with a
small floor so little parts survive as a few triangles), then a collapse
Decimate, which copes with the non-manifold pieces a plain edge collapse
gets stuck on. Materials are kept so make_scooter.py can read the colours.
"""
import sys

import bpy

argv = sys.argv[sys.argv.index("--") + 1:]
src, dst, budget = argv[0], argv[1], int(argv[2])

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=src)

meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
for o in meshes:
    o.data = o.data.copy()  # instanced meshes decimate independently

def tri_count(o):
    return sum(len(p.vertices) - 2 for p in o.data.polygons)

areas = {o: sum(p.area for p in o.data.polygons) * max(o.matrix_world.to_scale()) ** 2 for o in meshes}
total = sum(areas.values()) or 1.0
for o in meshes:
    target = max(6, round(budget * areas[o] / total))
    tris = tri_count(o)
    if tris <= target:
        continue
    m = o.modifiers.new("dec", "DECIMATE")
    m.decimate_type = "COLLAPSE"
    m.ratio = max(target / tris, 0.0005)
    m.use_collapse_triangulate = True
    bpy.context.view_layer.objects.active = o
    bpy.ops.object.modifier_apply(modifier=m.name)

print("TRIS", sum(tri_count(o) for o in meshes))
bpy.ops.export_scene.gltf(filepath=dst, export_format="GLB", export_apply=True)
