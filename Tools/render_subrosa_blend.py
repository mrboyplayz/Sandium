import bpy
import mathutils
import sys

output = sys.argv[sys.argv.index("--") + 1]
for obj in bpy.context.scene.objects:
    obj.hide_render = obj.type == 'ARMATURE'
scene = bpy.context.scene
scene.render.engine = 'BLENDER_WORKBENCH'
scene.display.shading.light = 'STUDIO'
scene.display.shading.show_shadows = True
scene.display.shading.show_cavity = True
scene.render.resolution_x = 700
scene.render.resolution_y = 900
scene.render.resolution_percentage = 100
scene.world = scene.world or bpy.data.worlds.new('World')
scene.world.color = (0.04, 0.04, 0.04)
camera_data = bpy.data.cameras.new('Preview Camera')
camera = bpy.data.objects.new('Preview Camera', camera_data)
scene.collection.objects.link(camera)
scene.camera = camera
camera.location = (0.0, -3.5, -0.42)
target = mathutils.Vector((0.0, 0.0, -0.42))
camera.rotation_euler = (target - camera.location).to_track_quat('-Z', 'Y').to_euler()
camera.data.lens = 62
scene.render.filepath = output
bpy.ops.render.render(write_still=True)
