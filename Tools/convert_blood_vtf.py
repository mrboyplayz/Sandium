"""Extract the largest DXT5 mip from selected user-supplied VTF textures."""

from io import BytesIO
from pathlib import Path
import struct

from PIL import Image


SOURCE = Path(r"C:\Users\junin\Downloads\z_city_content_1\materials")
DEST = Path(__file__).resolve().parents[1] / "Assets" / "blood"
FILES = {
    "drop": "particle/blood_drop.vtf",
    "mist": "particle/blood_mist/blood_mist.vtf",
    "splatter": "particle/blood_splatter/bloodsplatter.vtf",
    "stain": "homigrad/decals/blood1.vtf",
}


def dds_header(width: int, height: int) -> bytes:
    header = bytearray(128)
    header[:4] = b"DDS "
    struct.pack_into("<I", header, 4, 124)
    struct.pack_into("<I", header, 8, 0x1007)
    struct.pack_into("<II", header, 12, height, width)
    struct.pack_into("<I", header, 28, 1)
    struct.pack_into("<I", header, 76, 32)
    struct.pack_into("<I", header, 80, 0x4)
    header[84:88] = b"DXT5"
    struct.pack_into("<I", header, 108, 0x1000)
    return bytes(header)


def main() -> None:
    DEST.mkdir(parents=True, exist_ok=True)
    for name, relative in FILES.items():
        data = (SOURCE / relative).read_bytes()
        assert data[:4] == b"VTF\0"
        width, height = struct.unpack_from("<HH", data, 16)
        image_format = struct.unpack_from("<I", data, 52)[0]
        assert image_format == 15, (relative, image_format)
        byte_count = ((width + 3) // 4) * ((height + 3) // 4) * 16
        image = Image.open(BytesIO(dds_header(width, height) + data[-byte_count:]))
        image = image.convert("RGBA")
        if name == "drop":
            box = image.getchannel("A").getbbox()
            assert box is not None
            image = image.crop((max(0, box[0] - 2), max(0, box[1] - 2),
                                min(width, box[2] + 2), min(height, box[3] + 2)))
        target = DEST / f"{name}.png"
        image.save(target, optimize=True)
        print(target, image.size)
        if name == "mist":
            image.crop((96, 0, 128, 32)).save(DEST / "mist_frame.png", optimize=True)


if __name__ == "__main__":
    main()
