"""Rebuild src/ico2.ico with uncompressed BMP frames for the small sizes.

The original file held a single 256x256 PNG frame. Asking LoadImage for a 16 or
32 px icon then makes user32 decode that PNG through WIC, which maps
WindowsCodecs.dll (~2.3 MB) and IconCodecService.dll into the process for its
whole lifetime. Classic 32-bit BGRA DIB frames at the sizes Windows actually
requests are drawn without any codec; the 256 px PNG is kept for the shell's
large views.

Usage: python tools/make_icon.py <source.ico or .png> <out.ico>
"""
import io
import struct
import sys

from PIL import Image

SIZES = [16, 20, 24, 32, 40, 48, 64]


def load_master(path):
    img = Image.open(path)
    if getattr(img, "format", "") == "ICO":
        # Pick the largest frame.
        best = max(img.info.get("sizes", {img.size}), key=lambda s: s[0])
        img.size = best
    return img.convert("RGBA")


def dib_frame(img):
    w, h = img.size
    # BITMAPINFOHEADER; height counts XOR + AND masks.
    hdr = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    px = img.transpose(Image.FLIP_TOP_BOTTOM).tobytes("raw", "BGRA")
    mask_row = ((w + 31) // 32) * 4
    and_mask = bytes(mask_row * h)  # alpha channel carries transparency
    return hdr + px + and_mask


def png_frame(img):
    buf = io.BytesIO()
    img.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def main(src, out):
    master = load_master(src)
    frames = []
    for s in SIZES:
        frames.append((s, dib_frame(master.resize((s, s), Image.LANCZOS))))
    big = master if master.size == (256, 256) else master.resize((256, 256), Image.LANCZOS)
    frames.append((256, png_frame(big)))

    header = struct.pack("<HHH", 0, 1, len(frames))
    offset = 6 + 16 * len(frames)
    entries, blobs = b"", b""
    for size, data in frames:
        dim = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        blobs += data
        offset += len(data)
    with open(out, "wb") as f:
        f.write(header + entries + blobs)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
