#!/usr/bin/env python3
"""Draws the port's own icon (no game artwork): three dorsal plates glowing atomic blue over a dark
disc. Writes launcher/minuszero.png (256 px) and launcher/minuszero.ico (16 to 256 px).

    python packaging/windows/make_icon.py
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

OUT = Path(__file__).resolve().parent.parent.parent / 'launcher'
SIZE = 1024


def plate(cx, base, width, height):
    """A jagged dorsal plate: a pointed leaf with notched edges, pointing up."""
    w, h = width / 2, height
    return [(cx - w, base), (cx - w * 0.82, base - h * 0.30), (cx - w * 0.98, base - h * 0.42),
            (cx - w * 0.58, base - h * 0.62), (cx - w * 0.72, base - h * 0.72), (cx - w * 0.16, base - h * 0.90),
            (cx, base - h), (cx + w * 0.16, base - h * 0.90), (cx + w * 0.72, base - h * 0.72),
            (cx + w * 0.58, base - h * 0.62), (cx + w * 0.98, base - h * 0.42), (cx + w * 0.82, base - h * 0.30),
            (cx + w, base)]


def draw():
    image = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0))
    disc = ImageDraw.Draw(image)
    disc.ellipse((32, 32, SIZE - 32, SIZE - 32), fill=(14, 18, 28, 255), outline=(40, 70, 110, 255), width=18)
    plates = [plate(292, 770, 190, 330), plate(512, 770, 250, 540), plate(732, 770, 190, 330)]
    glow = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0))
    g = ImageDraw.Draw(glow)
    for points in plates:
        g.polygon(points, fill=(60, 170, 255, 255))
    glow = glow.filter(ImageFilter.GaussianBlur(38))
    image = Image.alpha_composite(image, glow)
    body = ImageDraw.Draw(image)
    for points in plates:
        body.polygon(points, fill=(150, 225, 255, 255))
    for points in plates:
        inner = [(x * 0.86 + sum(p[0] for p in points) / len(points) * 0.14,
                  y * 0.86 + sum(p[1] for p in points) / len(points) * 0.14) for x, y in points]
        body.polygon(inner, fill=(235, 250, 255, 255))
    # The ground line the plates rise from.
    body.rectangle((170, 770, SIZE - 170, 800), fill=(150, 225, 255, 255))
    mask = Image.new('L', (SIZE, SIZE), 0)
    ImageDraw.Draw(mask).ellipse((32, 32, SIZE - 32, SIZE - 32), fill=255)
    image.putalpha(Image.composite(image.getchannel('A'), mask, mask))
    return image


def main():
    OUT.mkdir(exist_ok=True)
    image = draw()
    image.resize((256, 256), Image.LANCZOS).save(OUT / 'minuszero.png')
    image.resize((256, 256), Image.LANCZOS).save(OUT / 'minuszero.ico',
                                                 sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64),
                                                        (128, 128), (256, 256)])
    print(f'Wrote {OUT / "minuszero.png"} and {OUT / "minuszero.ico"}')


if __name__ == '__main__':
    main()
