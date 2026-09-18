"""Convert a triangulated Wavefront OBJ to Sub Rosa CMO version 3."""
import argparse
import struct
from pathlib import Path


def convert(source: Path, output: Path) -> None:
    positions, texcoords, vertices, triangles = [], [], [], []
    for raw in source.read_text(errors="replace").splitlines():
        fields = raw.split()
        if not fields:
            continue
        if fields[0] == "v":
            positions.append(tuple(map(float, fields[1:4])))
        elif fields[0] == "vt":
            texcoords.append(tuple(map(float, fields[1:3])))
        elif fields[0] == "f":
            face = []
            for token in fields[1:]:
                parts = token.split("/")
                position = positions[int(parts[0]) - 1]
                uv = texcoords[int(parts[1]) - 1]
                face.append(len(vertices))
                vertices.append((*position, uv[0], 1.0 - uv[1], 0.0))
            for index in range(1, len(face) - 1):
                triangles.append((face[0], face[index], face[index + 1]))

    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("wb") as target:
        target.write(b"CMod")
        target.write(struct.pack("<II", 3, len(vertices)))
        for vertex in vertices:
            target.write(struct.pack("<6f", *vertex))
        target.write(struct.pack("<I", len(triangles)))
        for triangle in triangles:
            target.write(struct.pack("<I3III", 3, *triangle, 0, 0))
        target.write(struct.pack("<I", 0))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    convert(args.source, args.output)
