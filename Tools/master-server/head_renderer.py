"""Render shipped Sub Rosa CMod head and hair meshes for Discord portraits."""

from functools import lru_cache
from io import BytesIO
from math import cos, sin, radians, sqrt
from pathlib import Path
import struct

from PIL import Image, ImageDraw

MODEL_DIR = Path(__file__).with_name('head-models')
SIZE = 512
MENU_YAW_DEGREES = -20
# The game's 1024x640 preview is stretched to the 1267x868 menu screenshot.
MENU_SCREEN_STRETCH = (1024 / 640) / (1267 / 868)
PORTRAIT_SCALE = 1000.0
PORTRAIT_ORIGIN = (248.0, 236.0)
SKIN = [(178, 114, 95), (144, 71, 42), (222, 161, 142),
        (101, 50, 29), (212, 153, 117), (148, 77, 52)]
HAIR = [(25, 21, 20), (61, 34, 28), (111, 62, 41), (178, 98, 27),
        (226, 197, 100), (226, 89, 0), (76, 75, 74), (222, 215, 209)]
EYES = [(39, 19, 12), (77, 156, 167), (137, 72, 24), (106, 134, 33),
        (134, 145, 148), (62, 73, 29), (32, 80, 131), (209, 159, 45)]


@lru_cache(maxsize=32)
def load_cmo(name):
    data = (MODEL_DIR / (name + '.cmo')).read_bytes()
    if data[:4] != b'CMod' or struct.unpack_from('<I', data, 4)[0] != 3:
        raise ValueError('unsupported CMod model')
    count = struct.unpack_from('<I', data, 8)[0]
    if count > 10000:
        raise ValueError('oversized CMod model')
    vertices = [struct.unpack_from('<6f', data, 12 + i * 24)
                for i in range(count)]
    offset = 12 + count * 24
    face_count = struct.unpack_from('<I', data, offset)[0]
    if face_count > 10000:
        raise ValueError('oversized CMod model')
    faces = []
    for i in range(face_count):
        width, a, b, c, _, _ = struct.unpack_from('<6I', data, offset + 4 + i * 24)
        if width == 3 and max(a, b, c) < count:
            faces.append((a, b, c))
    return vertices, faces


def number(profile, field, maximum):
    try:
        return max(0, min(maximum, int(profile.get(field) or 0)))
    except (TypeError, ValueError):
        return 0


def render_head(profile):
    # RosaServer's value is 0=female, 1=male.
    gender = 'm' if number(profile, 'gender', 1) else 'f'
    head = number(profile, 'head', 4) + 1
    hair = number(profile, 'hair', 8) + 1
    skin_color = SKIN[number(profile, 'skinColor', 255) % len(SKIN)]
    hair_color = HAIR[number(profile, 'hairColor', 255) % len(HAIR)]
    eye_color = EYES[number(profile, 'eyeColor', 255) % len(EYES)]

    # The character menu draws head and hair with the same model transform.
    # Its camera is at (0, 0, 1), looking down -Z, with a perspective lens.
    yaw = radians(MENU_YAW_DEGREES)
    cy, sy = cos(yaw), sin(yaw)
    pitch = radians(0)
    cp, sp = cos(pitch), sin(pitch)
    portrait = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0))
    draw = ImageDraw.Draw(portrait)
    triangles = []
    meshes = []

    for name, material in ((f'{gender}head{head}', 'head'),
                           (f'{gender}hair{hair}', 'hair')):
        vertices, faces = load_cmo(name)
        transformed = []
        for x, y, z, u, v, _ in vertices:
            xr = x * cy + z * sy
            zr = z * cy - x * sy
            yr = y * cp - zr * sp
            zr = y * sp + zr * cp
            perspective = 1.0 / (1.0 - zr)
            transformed.append((xr, yr, zr, u, v,
                                xr * perspective,
                                yr * perspective * MENU_SCREEN_STRETCH))
        meshes.append((vertices, faces, transformed, material))

    # Keep one framing for every model, as the menu does. Auto-fitting each
    # hairstyle would change the apparent head size between profiles.
    scale = PORTRAIT_SCALE
    center_x, center_y = PORTRAIT_ORIGIN

    for vertices, faces, transformed, material in meshes:
        for a, b, c in faces:
            va, vb, vc = (transformed[i] for i in (a, b, c))
            ux, uy, uz = (vb[i] - va[i] for i in range(3))
            vx, vy, vz = (vc[i] - va[i] for i in range(3))
            nx, ny, nz = (uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx)
            length = sqrt(nx * nx + ny * ny + nz * nz) or 1
            nx, ny, nz = nx / length, ny / length, nz / length
            # The game files use clockwise winding; the visible side has -Z normals.
            if nz >= 0:
                continue
            # CMod uses inward-facing Y normals on the visible mesh, so an
            # overhead light contributes through -ny rather than +ny.
            light = max(0.32, min(1.27, 0.36 + 0.82 * max(0.0, -0.90 * ny - 0.42 * nz)))
            if material == 'hair':
                base = hair_color
            else:
                u = sum(v[3] for v in (va, vb, vc)) / 3
                v = sum(v[4] for v in (va, vb, vc)) / 3
                x = sum(vtx[0] for vtx in (vertices[i] for i in (a, b, c))) / 3
                y = sum(vtx[1] for vtx in (vertices[i] for i in (a, b, c))) / 3
                z = sum(vtx[2] for vtx in (vertices[i] for i in (a, b, c))) / 3
                if u > 0.65:
                    base = (231, 233, 229)  # eye white mesh
                elif 0.86 < v < 0.94 and 0.02 < abs(x) < 0.07 and abs(y) < 0.02 and z > 0.055:
                    base = eye_color  # iris mesh
                elif v < 0.83 and y > 0.012 and z > 0.05:
                    base = hair_color  # eyebrow mesh
                else:
                    base = skin_color
            color = tuple(max(0, min(255, round(channel * light))) for channel in base)
            points = [(center_x + v[5] * scale, center_y - v[6] * scale)
                      for v in (va, vb, vc)]
            depth = sum(v[2] for v in (va, vb, vc)) / 3
            triangles.append((depth, points, (*color, 255)))

    for _, points, color in sorted(triangles, key=lambda item: item[0]):
        draw.polygon(points, fill=color)
    portrait = portrait.resize((256, 256), Image.Resampling.LANCZOS)
    output = BytesIO()
    portrait.save(output, format='PNG')
    return output.getvalue()
