"""Build native Intern assets with the CMC loader's actual bind convention."""
import bpy
import numpy as np
import struct
import sys
import json
from pathlib import Path
from mathutils import Matrix, Vector

source, reference, destination = map(Path, sys.argv[sys.argv.index('--') + 1:][:3])
destination.mkdir(parents=True, exist_ok=True)
PARENT = [-1, 0, 1, 2, 2, 4, 5, 2, 7, 8, 0, 10, 11, 0, 13, 14]
NAMES = ['Pelvis', 'Spine', 'Chest', 'Head', 'Arm.L', 'Elbow.L', 'Hand.L',
         'Arm.R', 'Elbow.R', 'Hand.R', 'Leg.L', 'Knee.L', 'Foot.L', 'Leg.R', 'Knee.R', 'Foot.R']
data = reference.read_bytes()
assert data[:12] == b'CMod' + struct.pack('<II', 2, 16)
pivots = np.array([struct.unpack_from('<3f', data, 12 + i * 12) for i in range(16)])
# Canonical common-space bind: native spine hierarchy and forearm lengths.
# The native loader special-cases shoulder positions using a scratch humanoid;
# Playermodel.cpp restores these exported binds together with the mesh input.
bind = np.zeros((16, 3))
bind[0] = (0, -.1875, .0625)
for i in range(1, 16):
    pivot = pivots[i] * 1.125
    if i in (5, 6, 8, 9):
        pivot *= .328125 / np.linalg.norm(pivot)
    bind[i] = bind[PARENT[i]] + pivot

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=str(source))
arm = bpy.data.objects['Armature']
body = bpy.data.objects['Body']
arm.animation_data_clear()
arm.data.pose_position = 'REST'
for bone in arm.pose.bones:
    bone.matrix_basis.identity()
body.animation_data_clear()
if body.data.shape_keys:
    body.shape_key_clear()
bpy.context.view_layer.objects.active = body
bpy.ops.object.select_all(action='DESELECT')
body.select_set(True)
dec = body.modifiers.new('Native vertex budget', 'DECIMATE')
dec.ratio = .10
dec.use_collapse_triangulate = True
while body.modifiers.find(dec.name) > 0:
    bpy.ops.object.modifier_move_up(modifier=dec.name)
bpy.ops.object.modifier_apply(modifier=dec.name)
# Decimate can leave flipped triangles that Blender hides (double-sided)
# but the game culls. Force consistent outward winding before export.
# Also merge doubles from collapse to stop coplanar flicker on hands/feet.
bpy.context.view_layer.objects.active = body
bpy.ops.object.mode_set(mode='EDIT')
bpy.ops.mesh.select_all(action='SELECT')
try:
    bpy.ops.mesh.remove_doubles(threshold=0.0001)
except TypeError:
    bpy.ops.mesh.merge_by_distance(distance=0.0001)
bpy.ops.mesh.normals_make_consistent(inside=False)
bpy.ops.object.mode_set(mode='OBJECT')

def joint(name):
    return np.array(arm.matrix_world @ arm.data.bones[name].head_local)

# Associate anatomical sides by coordinates, not FBX labels. Source .L can
# occupy +X, whereas native bone 4 always occupies -X.
left = 'L' if joint('Arm.L')[0] < joint('Arm.R')[0] else 'R'
right = 'R' if left == 'L' else 'L'
sources = ['Hips', 'Spine', 'Chest', 'Head', f'Arm.{left}', f'Elbow.{left}', f'Wrist.{left}',
           f'Arm.{right}', f'Elbow.{right}', f'Wrist.{right}', f'Leg.{left}', f'Knee.{left}',
           f'Toes.{left}', f'Leg.{right}', f'Knee.{right}', f'Toes.{right}']
mapping = {name: i for i, name in enumerate(sources)}
mapping.update({'Neck': 2, 'Shoulder.L': 2, 'Shoulder.R': 2,
                'Tail': 0, 'Tail.001': 0, f'Pointer.{left}': 6, f'Pointer.{right}': 9})

def group_bone(name):
    if name.startswith(('Ear.', 'Tongue')): return 3
    if name.startswith('HoodieString'): return 2
    return mapping.get(name, 2)

children = {0: 1, 1: 2, 2: 3, 4: 5, 5: 6, 7: 8, 8: 9, 10: 11, 11: 12, 13: 14, 14: 15}
def frame(direction, front):
    y = direction / np.linalg.norm(direction)
    z = front - y * np.dot(front, y)
    if np.linalg.norm(z) < 1e-6:
        raise ValueError('Collinear anatomical frame')
    z /= np.linalg.norm(z)
    x = np.cross(y, z)
    return np.column_stack((x, y, z))

def align_rotation(src, dst):
    # Minimal-twist rotation mapping src -> dst. Used for limbs where the
    # old front-vector frames mirrored one side (left arm/leg twist).
    a = src / np.linalg.norm(src)
    b = dst / np.linalg.norm(dst)
    cross = np.cross(a, b)
    norm = np.linalg.norm(cross)
    dot = float(np.dot(a, b))
    if norm < 1e-9:
        if dot > 0:
            return np.eye(3)
        # 180 deg: pick any orthogonal axis.
        axis = np.array([1., 0., 0.]) if abs(a[0]) < 0.9 else np.array([0., 1., 0.])
        axis = axis - a * np.dot(axis, a)
        axis /= np.linalg.norm(axis)
        return -np.eye(3) + 2. * np.outer(axis, axis)
    skew = np.array([[0., -cross[2], cross[1]],
                     [cross[2], 0., -cross[0]],
                     [-cross[1], cross[0], 0.]])
    return np.eye(3) + skew + skew @ skew * (1. / (1. + dot))

# Spine keeps frame-based fronts (correct forward facing in v2); limbs use
# minimal twist (fixed arms in v4). Head bone stays minimal to preserve the
# v4 head CMO the user confirmed was fine.
FRAME_BONES = {0, 1, 2}

transforms = []
for i, name in enumerate(sources):
    start = joint(name)
    if i in children:
        src_dir = joint(sources[children[i]]) - start
        dst_dir = bind[children[i]] - bind[i]
    elif i in (6, 9):
        src_dir = start - joint(sources[PARENT[i]])
        dst_dir = bind[i] - bind[PARENT[i]]
    elif i in (12, 15):
        src_dir = np.array([0., -1., 0.]); dst_dir = np.array([0., 0., 1.])
    else:
        src_dir = np.array([0., 0., 1.]); dst_dir = np.array([0., 1., 0.])
    if i in FRAME_BONES:
        src_front = np.array([0., -1., 0.]) if i not in (12, 15) else np.array([0., 0., 1.])
        dst_front = np.array([0., 0., 1.]) if i not in (12, 15) else np.array([0., 1., 0.])
        rotation = frame(dst_dir, dst_front) @ frame(src_dir, src_front).T
    else:
        rotation = align_rotation(src_dir, dst_dir)
    scale = np.eye(3) * .18
    # Match joint-to-joint length along each source bone while retaining girth.
    if i in children:
        axis = src_dir / np.linalg.norm(src_dir)
        length_scale = np.linalg.norm(dst_dir) / np.linalg.norm(src_dir)
        scale += (length_scale - .18) * np.outer(axis, axis)
    transforms.append((rotation @ scale, start))

mesh = body.data
mesh.calc_loop_triangles()
worlds, weights = [], []
TORSO_LIFT = 0.06
BODY_ZC = 0.05
for vertex in mesh.vertices:
    influence = {}
    for assignment in vertex.groups:
        index = group_bone(body.vertex_groups[assignment.group].name)
        influence[index] = influence.get(index, 0.) + assignment.weight
    influence = dict(sorted(influence.items(), key=lambda pair: -pair[1])[:4])
    total = sum(influence.values())
    if total <= 0: raise ValueError('Unweighted vertex')
    influence = {k: v / total for k, v in influence.items() if v > 0}
    original = np.array(body.matrix_world @ vertex.co)
    world = sum(w * (bind[k] + transforms[k][0] @ (original - transforms[k][1])) for k,w in influence.items())
    # Raise the torso to vanilla height (body topped at 0.568 vs 0.626).
    dominant = max(influence, key=lambda b: influence[b])
    if dominant in (1, 2) or (world[1] > 0.10 and abs(world[0]) < 0.25 and world[1] < 0.60):
        world = world + np.array([0., TORSO_LIFT, 0.])
    # Entire model faces backwards in-game. Mirror front/back only (keep X
    # so left/right weights stay on the correct side).
    world = np.array([world[0], world[1], 2. * BODY_ZC - world[2]])
    worlds.append(world); weights.append(influence)

parts = {False: ([], [], {}), True: ([], [], {})}
uv_layer = mesh.uv_layers.active.data
for triangle in mesh.loop_triangles:
    is_head = sum(weights[v].get(3, 0) for v in triangle.vertices) / 3 >= .5
    vertices, faces, lookup = parts[is_head]
    face = []
    for loop in triangle.loops:
        vi = mesh.loops[loop].vertex_index
        uv = tuple(uv_layer[loop].uv)
        key = (vi, uv)
        if key not in lookup:
            lookup[key] = len(vertices)
            vertices.append((worlds[vi], uv, {3:1.} if is_head else weights[vi]))
        face.append(lookup[key])
    faces.append(face)

bv, bf, _ = parts[False]
hv, hf, _ = parts[True]
# Body stays mirrored (confirmed forward in-game). The head was fine
# unmirrored (v4) -- v14's full-mirror turned it backwards. Restore head
# orientation and seat it down onto the collar.
HEAD_DROP = 0.04
HEAD_ZC = 0.05
for idx, entry in enumerate(hv):
    w = entry[0]
    hv[idx] = (np.array([w[0], w[1] - HEAD_DROP, 2. * HEAD_ZC - w[2]]), entry[1], entry[2])
for face in bf:
    face[1], face[2] = face[2], face[1]
print(f'winding: body mirrored {len(bf)}, head restored {len(hf)}')
assert len(bf)*3 <= 8192, 'Native CMC expanded vertex budget exceeded'
max_error = 0.
with (destination/'intern_body.cmc').open('wb') as out:
    out.write(b'CMod'+struct.pack('<II',2,16)+pivots.astype('<f4').tobytes())
    out.write(struct.pack('<I',len(bv)))
    for world, uv, influence in bv:
        out.write(struct.pack('<3f', *world))
        reconstructed = np.zeros(3)
        for bone in range(16):
            local = ((world-bind[bone])/1.125).astype(np.float32)
            w = influence.get(bone,0.)
            out.write(struct.pack('<4f',*local,w))
            reconstructed += (local*1.125+bind[bone])*w
        max_error = max(max_error, float(np.linalg.norm(reconstructed-world)))
        out.write(struct.pack('<2f',*uv))
    out.write(struct.pack('<I',len(bf)))
    # Vanilla CMC/CMO uses clockwise faces (negative signed mesh volume).
    for face in bf: out.write(struct.pack('<3I',*reversed(face)))

def cmo(path, vertices, faces):
    with path.open('wb') as out:
        out.write(b'CMod'+struct.pack('<II',3,len(vertices)))
        for world, uv, _ in vertices:
            out.write(struct.pack('<6f',*(world-bind[3]),*uv,0.))
        out.write(struct.pack('<I',len(faces)))
        for face in faces: out.write(struct.pack('<I3Iii',3,*reversed(face),0,0))
        out.write(struct.pack('<I',0))
cmo(destination/'intern_head.cmo',hv,hf)
cmo(destination/'intern_empty_hair.cmo',[(bind[3],(0,0),{})]*3,[[0,1,2]])
(destination/'intern_bind.bin').write_bytes(bind.astype('<f4').tobytes())
assert max_error < 1e-5

# Native PNG decoder requires power-of-two dimensions. Keep UVs unchanged.
image = bpy.data.images.load(str(source.parent/'Intern.png'))
image.scale(2048,2048)
image.filepath_raw = str(destination/'intern.png')
image.file_format = 'PNG'
image.save()

bpy.ops.object.select_all(action='SELECT'); bpy.ops.object.delete(use_global=False)
axis = Matrix(((1,0,0,0),(0,0,-1,0),(0,1,0,0),(0,0,0,1)))
def bl(v): return axis.to_3x3() @ Vector(v)
rig = bpy.data.objects.new('Sub Rosa Skeleton', bpy.data.armatures.new('Sub Rosa Skeleton'))
bpy.context.collection.objects.link(rig)
bpy.context.view_layer.objects.active = rig; rig.select_set(True)
bpy.ops.object.mode_set(mode='EDIT')
for i,name in enumerate(NAMES):
    bone = rig.data.edit_bones.new(name)
    # Preserve the native identity basis, including its roll, in Blender space.
    mat = Matrix.Translation(Vector(bind[i]))
    bone.matrix = axis @ mat
    bone.length = .12
    if PARENT[i]>=0: bone.parent = rig.data.edit_bones[NAMES[PARENT[i]]]
bpy.ops.object.mode_set(mode='OBJECT')
mat = bpy.data.materials.new('Intern original texture'); mat.use_nodes = True
tex = mat.node_tree.nodes.new('ShaderNodeTexImage'); tex.image = image
mat.node_tree.links.new(tex.outputs['Color'],mat.node_tree.nodes.get('Principled BSDF').inputs['Base Color'])
image.pack()
for title,vertices,faces in [('Intern Body',bv,bf),('Intern Head',hv,hf)]:
    new_mesh=bpy.data.meshes.new(title)
    new_mesh.from_pydata([bl(v[0]) for v in vertices],[],faces); new_mesh.update()
    obj=bpy.data.objects.new(title,new_mesh); bpy.context.collection.objects.link(obj)
    uv=new_mesh.uv_layers.new()
    for loop in new_mesh.loops: uv.data[loop.index].uv=vertices[loop.vertex_index][1]
    for i,name in enumerate(NAMES):
        group=obj.vertex_groups.new(name=name)
        for index,v in enumerate(vertices):
            if v[2].get(i,0)>0: group.add([index],v[2][i],'REPLACE')
    modifier=obj.modifiers.new('Native skeleton','ARMATURE'); modifier.object=rig
    obj.parent=rig; obj.data.materials.append(mat)
rig.show_in_front=True
bpy.ops.wm.save_as_mainfile(filepath=str(destination/'Intern_Rebuilt.blend'))
report={'body_vertices':len(bv),'body_triangles':len(bf),'head_vertices':len(hv),
        'export_bind_roundtrip_max_error':max_error,'texture_dimensions':list(image.size),
        'native_binds':bind.tolist(),'source_bones':sources}
(destination/'validation.json').write_text(json.dumps(report,indent=2))
print(json.dumps(report))
