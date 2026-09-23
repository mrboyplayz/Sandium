import bpy
import sys
from pathlib import Path

argv = sys.argv
if "--" not in argv:
    raise SystemExit("expected -- <input.fbx> <output.obj>")
args = argv[argv.index("--") + 1:]
source = Path(args[0])
target = Path(args[1])
target.parent.mkdir(parents=True, exist_ok=True)

bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()
bpy.ops.import_scene.fbx(filepath=str(source))

for obj in bpy.context.scene.objects:
    obj.select_set(obj.type == "MESH")
bpy.context.view_layer.objects.active = next(
    (obj for obj in bpy.context.scene.objects if obj.type == "MESH"), None)

# The Sketchfab mesh is authored as a display asset. Apply a compact scale so it
# lands in Sub Rosa hand-item size; final grip is tuned in ItemType offsets.
for obj in bpy.context.selected_objects:
    obj.scale = (0.018, 0.018, 0.018)
    obj.rotation_euler[0] = 0.0
    obj.rotation_euler[1] = 0.0
    obj.rotation_euler[2] = 0.0

bpy.ops.wm.obj_export(filepath=str(target), export_selected_objects=True)
