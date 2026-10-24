#!/usr/bin/env python3
"""Converts the raw frame dumps written with MZ_FRAME_DUMP_TRIGGER (out/dump/<name>_<n>_<w>x<h>.raw,
32-bit pixels) into PNG files beside them.

    python tools/raw_to_png.py [out/dump] [--bgra]
"""
import re
import sys
from pathlib import Path

from PIL import Image


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    folder = Path(args[0] if args else 'out/dump')
    mode = 'BGRA' if '--bgra' in sys.argv else 'RGBA'
    for raw in sorted(folder.glob('*.raw')):
        match = re.search(r'_(\d+)x(\d+)\.raw$', raw.name)
        if not match:
            continue
        w, h = int(match.group(1)), int(match.group(2))
        data = raw.read_bytes()
        if len(data) < w * h * 4:
            continue
        image = Image.frombytes('RGBA', (w, h), data[:w * h * 4], 'raw', mode).convert('RGB')
        image.save(raw.with_suffix('.png'))
        print(raw.with_suffix('.png'))


if __name__ == '__main__':
    main()
