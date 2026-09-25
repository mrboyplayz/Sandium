"""Retarget CandyCreaturez' Intern FBX to Sub Rosa's 16-part character format.

Run with Blender:
  blender --background --python convert_intern_playermodel.py -- source.fbx vanilla.cmc output_dir
"""
import bpy
import mathutils
import numpy as np
import os
import struct
import sys


FBX, VANILLA, OUTPUT = sys.argv[sys.argv.index("--") + 1:sys.argv.index("--") + 4]
SCALE = 0.18
SR_SCALE = 1.125
PARENT = [-1, 0, 1, 1, 1, 4, 5, 1, 7, 8, 0, 10, 11, 0, 13, 14]
TARGET_NAMES = ["Hips", "Chest", "Head", "Head", "Arm.L", "Elbow.L", "Wrist.L",
                "Arm.R", "Elbow.R", "Wrist.R", "Leg.L", "Knee.L", "Toes.L",
                "Leg.R", "Knee.R", "Toes.R"]


def read_vanilla(path):
    data = open(path, "rb").read()
    if data[:4] != b"CMod" or struct.unpack_from("<II", data, 4) != (2, 16):
        raise RuntimeError("reference body is not a Sub Rosa CMC v2")
    pivots = [struct.unpack_from("<3f", data, 12 + i * 12) for i in range(16)]
    count = struct.unpack_from("<I", data, 0xCC)[0]
    samples = [[] for _ in range(16)]
    for index in range(count):
        record = 0xD0 + index * 276
        world = np.array((*struct.unpack_from("<3f", data, record), 1.0))
        weighted = []
        for bone in range(16):
            off = record + 12 + bone * 16
            local = np.array((*struct.unpack_from("<3f", data, off), 1.0))
            weight = struct.unpack_from("<f", data, off + 12)[0]
            # Every non-zero influence stores the vertex in that bone's true
            # local bind space. Requiring a perfectly rigid vertex left the
            # chest with no samples, forcing an invalid translation-only bind
            # and producing the long spikes seen in game.
            if weight > 0.001:
                weighted.append((bone, local))
        if len(weighted) == 1:
            bone, local = weighted[0]
            local[:3] *= SR_SCALE
            samples[bone].append((local[:3], world[:3]))

    binds = [None] * 16
    for bone in range(16):
        print(f"reference bone {bone}: {len(samples[bone])} rigid samples")
        if len(samples[bone]) >= 3:
            p = np.array([pair[0] for pair in samples[bone]])
            q = np.array([pair[1] for pair in samples[bone]])
            cp, cq = p.mean(axis=0), q.mean(axis=0)
            u, _, vt = np.linalg.svd((p - cp).T @ (q - cq))
            rotation = vt.T @ u.T
            if np.linalg.det(rotation) < 0:
                vt[-1, :] *= -1
                rotation = vt.T @ u.T
            matrix = np.eye(4)
            matrix[:3, :3] = rotation
            matrix[:3, 3] = cq - rotation @ cp
            binds[bone] = matrix
        else:
            parent = PARENT[bone]
            base = np.eye(4) if parent < 0 else binds[parent].copy()
            translation = np.eye(4)
            translation[:3, 3] = np.array(pivots[bone]) * SR_SCALE
            binds[bone] = base @ translation
    return pivots, binds


def mapped_target(group_name):
    exact = {
        # Shipped CMC bodies do not weight any geometry to bone 1. Their
        # torso/shoulder shell follows the pelvis/root (0); bone 1 is an IK
        # helper whose file-space bind cannot be recovered from a CMC.
        "Hips": 0, "Spine": 0, "Chest": 0, "Neck": 0,
        "Head": 2, "Shoulder.L": 0, "Arm.L": 4, "Elbow.L": 5,
        "Wrist.L": 6, "Pointer.L": 6, "Shoulder.R": 0, "Arm.R": 7,
        "Elbow.R": 8, "Wrist.R": 9, "Pointer.R": 9,
        "Leg.L": 10, "Knee.L": 11, "Toes.L": 12,
        "Leg.R": 13, "Knee.R": 14, "Toes.R": 15,
        "Tail": 0, "Tail.001": 0,
    }
    if group_name in exact:
        return exact[group_name]
    if group_name.startswith("Ear.") or group_name.startswith("Tongue"):
        return 2
    if group_name.startswith("HoodieString"):
        return 0
    return None


def anatomical_frame(origin, along, forward_hint):
    along = np.asarray(along, dtype=float)
    along /= np.linalg.norm(along)
    forward = np.asarray(forward_hint, dtype=float)
    forward -= along * np.dot(forward, along)
    if np.linalg.norm(forward) < 1e-5:
        forward = np.array((0.0, 0.0, 1.0))
        forward -= along * np.dot(forward, along)
    forward /= np.linalg.norm(forward)
    right = np.cross(along, forward)
    right /= np.linalg.norm(right)
    forward = np.cross(right, along)
    matrix = np.eye(4)
    matrix[:3, 0] = right
    matrix[:3, 1] = along
    matrix[:3, 2] = forward
    matrix[:3, 3] = origin
    return matrix


def triangulated_entries(obj, armature, binds):
    mesh = obj.data
    mesh.calc_loop_triangles()
    uv_layer = mesh.uv_layers.active.data if mesh.uv_layers.active else None
    source_frames = {}
    target_frames = {}
    target_children = {0: 2, 2: 2, 4: 5, 5: 6, 6: 6, 7: 8, 8: 9, 9: 9,
                       10: 11, 11: 12, 12: 12, 13: 14, 14: 15, 15: 15}
    for target, name in enumerate(TARGET_NAMES):
        bone = armature.data.bones[name]
        source_origin = np.array((armature.matrix_world @ bone.head_local)[:], dtype=float)
        source_tail = np.array((armature.matrix_world @ bone.tail_local)[:], dtype=float)
        source_frames[target] = anatomical_frame(source_origin, source_tail - source_origin,
                                                  (0.0, -1.0, 0.0))
        target_origin = binds[target][:3, 3]
        child = target_children.get(target, target)
        if child != target:
            target_along = binds[child][:3, 3] - target_origin
        elif target in (6, 9, 12, 15):
            parent = PARENT[target]
            target_along = target_origin - binds[parent][:3, 3]
        else:
            target_along = binds[target][:3, 1]
        target_frames[target] = anatomical_frame(target_origin, target_along, (0.0, 0.0, 1.0))

    vertex_data = {}
    for vertex in mesh.vertices:
        influences = {}
        for assignment in vertex.groups:
            name = obj.vertex_groups[assignment.group].name
            target = mapped_target(name)
            if target is not None:
                influences[target] = influences.get(target, 0.0) + assignment.weight
        if not influences:
            influences = {1: 1.0}
        total = sum(influences.values())
        influences = {bone: weight / total for bone, weight in influences.items()}
        source_world = np.array((*((obj.matrix_world @ vertex.co)[:]), 1.0))
        target_world = np.zeros(4)
        target_world[3] = 1.0
        target_world[:3] = 0.0
        for bone, weight in influences.items():
            local = np.linalg.inv(source_frames[bone]) @ source_world
            local[:3] *= SCALE
            placed = target_frames[bone] @ local
            target_world[:3] += placed[:3] * weight
        vertex_data[vertex.index] = (target_world, influences)

    body_vertices, body_lookup, body_faces = [], {}, []
    head_vertices, head_lookup, head_faces = [], {}, []
    head_inverse = np.linalg.inv(binds[2])
    for tri in mesh.loop_triangles:
        head_score = sum(vertex_data[mesh.loops[loop].vertex_index][1].get(2, 0.0)
                         for loop in tri.loops) / 3.0
        is_head = head_score >= 0.5
        output, lookup, faces = ((head_vertices, head_lookup, head_faces) if is_head
                                  else (body_vertices, body_lookup, body_faces))
        face = []
        for loop_index in tri.loops:
            source_index = mesh.loops[loop_index].vertex_index
            uv = uv_layer[loop_index].uv[:] if uv_layer else (0.0, 0.0)
            key = (source_index, round(uv[0], 7), round(uv[1], 7))
            if key not in lookup:
                world, influences = vertex_data[source_index]
                position = head_inverse @ world if is_head else world
                lookup[key] = len(output)
                output.append((position[:3], uv, influences, world[:3]))
            face.append(lookup[key])
        # Both anatomical frames are right-handed, so this conversion does not
        # mirror the mesh. Preserve Blender's triangle winding; reversing it
        # made random-looking surfaces disappear under native back-face culling.
        faces.append(tuple(face))
    return body_vertices, body_faces, head_vertices, head_faces


def write_cmc(path, pivots, binds, vertices, faces):
    with open(path, "wb") as out:
        out.write(b"CMod" + struct.pack("<II", 2, 16))
        for pivot in pivots:
            out.write(struct.pack("<3f", *pivot))
        out.write(struct.pack("<I", len(vertices)))
        inverses = [np.linalg.inv(bind) for bind in binds]
        for _, uv, influences, world in vertices:
            out.write(struct.pack("<3f", *world))
            world4 = np.array((*world, 1.0))
            for bone in range(16):
                local = inverses[bone] @ world4
                weight = influences.get(bone, 0.0)
                out.write(struct.pack("<4f", *(local[:3] / SR_SCALE), weight))
            out.write(struct.pack("<2f", uv[0], uv[1]))
        out.write(struct.pack("<I", len(faces)))
        for face in faces:
            out.write(struct.pack("<3I", *face))


def write_cmo(path, vertices, faces):
    with open(path, "wb") as out:
        out.write(b"CMod" + struct.pack("<II", 3, len(vertices)))
        for position, uv, _, _ in vertices:
            out.write(struct.pack("<6f", *position, uv[0], uv[1], 0.0))
        out.write(struct.pack("<I", len(faces)))
        for face in faces:
            out.write(struct.pack("<I3III", 3, *face, 0, 0))
        out.write(struct.pack("<I", 0))


def write_editable_blend(path, texture_path, binds, body_vertices, body_faces,
                         head_vertices, head_faces):
    # Keep an editable, action-free reference file. The distributed FBX opens
    # on an authored pose/action; that is why it appears folded in Blender even
    # though its REST mesh is valid.
    bpy.ops.object.mode_set(mode='OBJECT') if bpy.context.object and bpy.context.object.mode != 'OBJECT' else None
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)

    arm_data = bpy.data.armatures.new("Sub Rosa Skeleton")
    arm_obj = bpy.data.objects.new("Sub Rosa Skeleton", arm_data)
    bpy.context.collection.objects.link(arm_obj)
    bpy.context.view_layer.objects.active = arm_obj
    arm_obj.select_set(True)
    bpy.ops.object.mode_set(mode='EDIT')
    bone_names = ["Pelvis", "ChestIK", "Head", "Face", "Arm.L", "Elbow.L", "Hand.L",
                  "Arm.R", "Elbow.R", "Hand.R", "Leg.L", "Knee.L", "Foot.L",
                  "Leg.R", "Knee.R", "Foot.R"]
    edit_bones = []
    def blender_space(value):
        # CMC is Y-up; Blender is Z-up.
        return mathutils.Vector((float(value[0]), -float(value[2]), float(value[1])))
    children = {0: 2, 1: 2, 2: 3, 3: 3, 4: 5, 5: 6, 6: 6, 7: 8, 8: 9, 9: 9,
                10: 11, 11: 12, 12: 12, 13: 14, 14: 15, 15: 15}
    for index, name in enumerate(bone_names):
        bone = arm_data.edit_bones.new(name)
        origin = blender_space(binds[index][:3, 3])
        child = children[index]
        if child != index:
            tail = blender_space(binds[child][:3, 3])
        elif PARENT[index] >= 0:
            direction = origin - blender_space(binds[PARENT[index]][:3, 3])
            tail = origin + (direction.normalized() * max(direction.length * 0.45, 0.08))
        else:
            tail = origin + mathutils.Vector((0.0, 0.12, 0.0))
        if (tail - origin).length < 0.01:
            tail = origin + mathutils.Vector((0.0, 0.08, 0.0))
        bone.head, bone.tail = origin, tail
        edit_bones.append(bone)
    for index, parent in enumerate(PARENT):
        if parent >= 0:
            edit_bones[index].parent = edit_bones[parent]
    bpy.ops.object.mode_set(mode='OBJECT')

    def make_mesh(name, vertices, faces, head_only=False):
        mesh = bpy.data.meshes.new(name)
        mesh.from_pydata([tuple(blender_space(entry[3])) for entry in vertices], [], faces)
        mesh.update()
        obj = bpy.data.objects.new(name, mesh)
        bpy.context.collection.objects.link(obj)
        uv_layer = mesh.uv_layers.new(name="UVMap")
        for polygon in mesh.polygons:
            for loop_index in polygon.loop_indices:
                uv_layer.data[loop_index].uv = vertices[mesh.loops[loop_index].vertex_index][1]
        for bone_index, bone_name in enumerate(bone_names):
            group = obj.vertex_groups.new(name=bone_name)
            for vertex_index, entry in enumerate(vertices):
                weight = 1.0 if head_only and bone_index == 2 else entry[2].get(bone_index, 0.0)
                if weight > 0.0001:
                    group.add([vertex_index], weight, 'REPLACE')
        modifier = obj.modifiers.new(name="Sub Rosa Skeleton", type='ARMATURE')
        modifier.object = arm_obj
        obj.parent = arm_obj
        return obj

    body_obj = make_mesh("Intern Body", body_vertices, body_faces)
    head_obj = make_mesh("Intern Head", head_vertices, head_faces, True)
    material = bpy.data.materials.new("Intern")
    material.use_nodes = True
    image = bpy.data.images.load(texture_path, check_existing=True)
    image.pack()
    nodes = material.node_tree.nodes
    texture = nodes.new("ShaderNodeTexImage")
    texture.image = image
    material.node_tree.links.new(texture.outputs["Color"], nodes.get("Principled BSDF").inputs["Base Color"])
    material.node_tree.links.new(texture.outputs["Alpha"], nodes.get("Principled BSDF").inputs["Alpha"])
    body_obj.data.materials.append(material)
    head_obj.data.materials.append(material)
    arm_obj.show_in_front = True
    bpy.context.view_layer.objects.active = body_obj
    body_obj.select_set(True)
    bpy.ops.wm.save_as_mainfile(filepath=path)


os.makedirs(OUTPUT, exist_ok=True)
pivots, target_binds = read_vanilla(VANILLA)
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=FBX)
body = bpy.data.objects["Body"]
armature = bpy.data.objects["Armature"]
# Sub Rosa's CMC upload scratch space is sized around the shipped ~2,000
# vertices. Collapse the high-detail VRChat mesh before UV seam expansion so
# the retargeted body stays inside that native budget.
bpy.context.view_layer.objects.active = body
bpy.ops.object.select_all(action="DESELECT")
body.select_set(True)
if body.data.shape_keys:
    body.shape_key_clear()
decimate = body.modifiers.new(name="Sub Rosa vertex budget", type="DECIMATE")
decimate.ratio = 0.20
decimate.use_collapse_triangulate = True
while body.modifiers.find(decimate.name) > 0:
    bpy.ops.object.modifier_move_up(modifier=decimate.name)
bpy.ops.object.modifier_apply(modifier=decimate.name)
body_vertices, body_faces, head_vertices, head_faces = triangulated_entries(body, armature, target_binds)
write_cmc(os.path.join(OUTPUT, "intern_body.cmc"), pivots, target_binds, body_vertices, body_faces)
write_cmo(os.path.join(OUTPUT, "intern_head.cmo"), head_vertices, head_faces)

# A valid but degenerate model suppresses the normal hairstyle for the preset.
empty = [((0.0, 0.0, 0.0), (0.0, 0.0), {}, (0.0, 0.0, 0.0)) for _ in range(3)]
write_cmo(os.path.join(OUTPUT, "intern_empty_hair.cmo"), empty, [(0, 1, 2)])
write_editable_blend(os.path.join(OUTPUT, "Intern_SubRosa.blend"),
                     os.path.join(os.path.dirname(FBX), "Intern.png"), target_binds,
                     body_vertices, body_faces, head_vertices, head_faces)
print(f"Intern conversion: body {len(body_vertices)} verts/{len(body_faces)} tris; "
      f"head {len(head_vertices)} verts/{len(head_faces)} tris")
