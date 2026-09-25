import bpy
import json
import sys


source = sys.argv[sys.argv.index("--") + 1]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=source)

report = {"objects": [], "armatures": []}
for obj in bpy.context.scene.objects:
    entry = {
        "name": obj.name,
        "type": obj.type,
        "parent": obj.parent.name if obj.parent else None,
        "location": list(obj.location),
        "scale": list(obj.scale),
    }
    if obj.type == "MESH":
        entry.update({
            "vertices": len(obj.data.vertices),
            "polygons": len(obj.data.polygons),
            "materials": [slot.material.name if slot.material else None for slot in obj.material_slots],
            "vertex_groups": [group.name for group in obj.vertex_groups],
            "bounds": {
                "min": [min((obj.matrix_world @ v.co)[axis] for v in obj.data.vertices) for axis in range(3)],
                "max": [max((obj.matrix_world @ v.co)[axis] for v in obj.data.vertices) for axis in range(3)],
            },
            "modifiers": [{"name": mod.name, "type": mod.type,
                           "object": getattr(getattr(mod, "object", None), "name", None)}
                          for mod in obj.modifiers],
        })
    report["objects"].append(entry)
    if obj.type == "ARMATURE":
        report["armatures"].append({
            "name": obj.name,
            "bones": [{
                "name": bone.name,
                "parent": bone.parent.name if bone.parent else None,
                "head": list(bone.head_local),
                "tail": list(bone.tail_local),
            } for bone in obj.data.bones],
        })

print("INTERN_FBX_REPORT_BEGIN")
print(json.dumps(report, indent=2))
print("INTERN_FBX_REPORT_END")
