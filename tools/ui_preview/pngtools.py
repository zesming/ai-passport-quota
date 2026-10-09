"""Stdlib PNG helpers for the preview: shrink the renderer's uncompressed PNGs and tile them into
contact sheets. Only reads PNGs the renderer writes (8-bit RGB, filter 0)."""

import struct
import zlib
from pathlib import Path


def _chunks(data):
    at = 8
    while at < len(data):
        length, kind = struct.unpack(">I4s", data[at : at + 8])
        yield kind, data[at + 8 : at + 8 + length]
        at += 12 + length


def read_png(path):
    data = Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path} is not a PNG")
    width = height = 0
    idat = b""
    for kind, body in _chunks(data):
        if kind == b"IHDR":
            width, height, depth, color = struct.unpack(">IIBB", body[:10])
            if depth != 8 or color != 2:
                raise ValueError(f"{path}: only 8-bit RGB is supported")
        elif kind == b"IDAT":
            idat += body
    raw = zlib.decompress(idat)
    stride = width * 3 + 1
    rows = []
    for y in range(height):
        if raw[y * stride] != 0:
            raise ValueError(f"{path}: filtered rows are not supported")
        rows.append(raw[y * stride + 1 : (y + 1) * stride])
    return width, height, rows


def write_png(path, width, height, rows):
    def chunk(kind, body):
        crc = zlib.crc32(kind + body) & 0xFFFFFFFF
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", crc)

    raw = b"".join(b"\x00" + row for row in rows)
    Path(path).write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def shrink(directory):
    for path in sorted(Path(directory).glob("*.png")):
        width, height, rows = read_png(path)
        write_png(path, width, height, rows)


def contact_sheets(directory, columns=4, per_sheet=8, gap=8):
    """Tile the PNGs of a directory, in name order, into sheet-N.png files next to them."""
    paths = [p for p in sorted(Path(directory).glob("*.png")) if not p.name.startswith("sheet-")]
    sheets = []
    for number, start in enumerate(range(0, len(paths), per_sheet), 1):
        group = paths[start : start + per_sheet]
        images = [read_png(p) for p in group]
        cell_w, cell_h = images[0][0], images[0][1]
        rows_needed = (len(images) + columns - 1) // columns
        width = columns * cell_w + (columns + 1) * gap
        height = rows_needed * cell_h + (rows_needed + 1) * gap
        canvas = [bytearray(b"\x40\x40\x40" * width) for _ in range(height)]
        for index, (_, _, rows) in enumerate(images):
            x = gap + (index % columns) * (cell_w + gap)
            y = gap + (index // columns) * (cell_h + gap)
            for r, row in enumerate(rows):
                canvas[y + r][x * 3 : (x + cell_w) * 3] = row
        target = Path(directory) / f"sheet-{number}.png"
        write_png(target, width, height, [bytes(r) for r in canvas])
        sheets.append((target, [p.stem for p in group]))
    return sheets
