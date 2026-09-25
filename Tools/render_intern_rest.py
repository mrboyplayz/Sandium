import bpy
import mathutils
import sys

source, output = sys.argv[sys.argv.index("--") + 1:sys.argv.index("--") + 3]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=source)
armature = bpy.data.objects.get("Armature")
body = bpy.data.objects.get("Body")
if armature:
    armature.data.pose_position = 'REST'
    armature.hide_render = True
for obj in list(bpy.context.scene.objects):
    obj.hide_render = obj != body

scene = bpy.context.scene
scene.render.engine = 'BLENDER_WORKBENCH'
scene.display.shading.light = 'STUDIO'
scene.display.shading.show_shadows = True
scene.display.shading.show_cavity = True
scene.display.shading.cavity_type = 'WORLD'
scene.render.resolution_x = 700
scene.render.resolution_y = 900
scene.render.resolution_percentage = 100
scene.render.image_settings.file_format = 'PNG'
scene.render.film_transparent = False
scene.world = bpy.data.worlds.new('World')
scene.world.color = (0.04, 0.04, 0.04)

camera_data = bpy.data.cameras.new('Camera')
camera = bpy.data.objects.new('Camera', camera_data)
scene.collection.objects.link(camera)
scene.camera = camera
camera.location = (0.0, -16.0, 4.6)
target = mathutils.Vector((0.0, 0.0, 4.6))
camera.rotation_euler = (target - camera.location).to_track_quat('-Z', 'Y').to_euler()
camera.data.lens = 58
scene.render.filepath = output
bpy.ops.render.render(write_still=True)
