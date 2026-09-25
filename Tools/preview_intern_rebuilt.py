import bpy
import sys
from mathutils import Vector

scene = bpy.context.scene
scene.render.engine = 'BLENDER_WORKBENCH'
scene.display.shading.color_type = 'TEXTURE'
scene.display.shading.light = 'STUDIO'
scene.display.shading.show_shadows = True
scene.display.shading.show_cavity = True
for obj in scene.objects:
    obj.hide_render = obj.type == 'ARMATURE'
    if obj.type == 'MESH':
        for mat in obj.data.materials:
            for node in mat.node_tree.nodes:
                if node.type == 'TEX_IMAGE':
                    mat.node_tree.nodes.active = node
camera = bpy.data.objects.new('Preview', bpy.data.cameras.new('Preview'))
scene.collection.objects.link(camera)
scene.camera = camera
camera.location = (0, -4.6, -.2)
camera.rotation_euler = (Vector((0, 0, -.2))-camera.location).to_track_quat('-Z','Y').to_euler()
camera.data.lens = 58
scene.render.resolution_x = 750
scene.render.resolution_y = 1000
scene.render.resolution_percentage = 100
scene.render.filepath = sys.argv[sys.argv.index('--')+1]
bpy.ops.render.render(write_still=True)
