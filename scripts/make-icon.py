"""Regenerate the geometric app icon using only Python's standard library."""
from pathlib import Path
import struct


def pixel(x, y):
    # Three small faders; a quiet, legible silhouette at tray size.
    for line, knob in ((0.28, 0.38), (0.50, 0.66), (0.72, 0.45)):
        if abs(x - line) < 0.023 and 0.18 < y < 0.82:
            return True
        if abs(x - line) < 0.075 and abs(y - knob) < 0.045:
            return True
    return False


def image(size):
    pixels = bytearray()
    for row in range(size - 1, -1, -1):
        for col in range(size):
            hits = sum(pixel((col + (sx + 0.5) / 4) / size,
                             (row + (sy + 0.5) / 4) / size)
                       for sx in range(4) for sy in range(4))
            pixels.extend((245, 245, 245, round(hits / 16 * 255)))
    mask = bytes(((size + 31) // 32) * 4 * size)
    header = struct.pack('<IiiHHIIiiII', 40, size, size * 2, 1, 32, 0, len(pixels), 0, 0, 0, 0)
    return header + pixels + mask


sizes = (16, 20, 24, 32, 48, 64, 128, 256)
images = [image(size) for size in sizes]
offset = 6 + 16 * len(sizes)
directory = bytearray(struct.pack('<HHH', 0, 1, len(sizes)))
for size, data in zip(sizes, images):
    directory.extend(struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32, len(data), offset))
    offset += len(data)
target = Path(__file__).resolve().parent.parent / 'resources' / 'app.ico'
target.write_bytes(directory + b''.join(images))
print(target)
