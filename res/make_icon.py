"""Generates the Wallpaper Plus logo: a monitor on a stand showing a dusk wallpaper
(two ridges against a sky that warms to the app's amber), with a plus-shaped star.

Writes res/icon.ico (all sizes), res/icon-256.png and res/logo.svg (the same drawing,
inlined in ui/index.html). Sizes of 24 px and below use a simplified drawing so the
star and the stand stay crisp in the tray. Needs Pillow. Run: python res/make_icon.py"""
import io
import math
import os
import re
import struct

from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))

SKY = [(0.00, (34, 24, 54)), (0.36, (176, 78, 98)), (0.64, (244, 184, 96)), (1.00, (250, 214, 150))]  # plum -> coral -> app amber
BACK_RIDGE = (122, 52, 80)
FRONT_RIDGE = (22, 15, 28)
STAR = (255, 244, 222)
STAND = (125, 120, 133)

# Everything is laid out on a 32 x 32 grid.
FULL = dict(screen=(2, 4, 28, 19, 3.0), star=(22.2, 9.4, 2.3, 0.8), glow=4.6,
            neck=(14.5, 23, 3, 3.6), base=(10, 26.4, 12, 2.2), ridges=2)
# Star centred on a pixel centre at 16 px so it lands as a sharp 3 x 3 cross.
SMALL = dict(screen=(1, 2, 30, 20, 3.4), star=(23, 9, 3, 2), glow=0,
             neck=(14, 22, 4, 3), base=(8, 25, 16, 4), ridges=1)


def back_ridge(x):
    return 16.4 - 3.0 * math.exp(-((x - 9.5) / 5.5) ** 2) - 1.1 * math.exp(-((x - 27) / 4) ** 2)


def front_ridge(x):
    return 20.2 - 2.8 * math.exp(-((x - 23) / 6) ** 2) - 0.9 * math.exp(-((x - 3) / 4) ** 2)


def ridge_points(fn, x0, x1, bottom, steps=48):
    pts = [(x0 + (x1 - x0) * i / steps, 0.0) for i in range(steps + 1)]
    pts = [(x, fn(x)) for x, _ in pts]
    return pts + [(x1, bottom), (x0, bottom)]


def star_rects(cx, cy, arm, thick):
    h = thick / 2
    return [(cx - arm, cy - h, cx + arm, cy + h), (cx - h, cy - arm, cx + h, cy + arm)]


def sky_color(t):
    for (t0, c0), (t1, c1) in zip(SKY, SKY[1:]):
        if t <= t1:
            k = (t - t0) / (t1 - t0)
            return tuple(round(a + (b - a) * k) for a, b in zip(c0, c1))
    return SKY[-1][1]


def render(size):
    spec = SMALL if size <= 24 else FULL
    ss = 8
    px = size * ss
    s = px / 32

    def box(x, y, w, h):
        return [x * s, y * s, (x + w) * s, (y + h) * s]

    img = Image.new("RGBA", (px, px), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rectangle(box(*spec["neck"]), fill=STAND)
    bx, by, bw, bh = spec["base"]
    d.rounded_rectangle(box(bx, by, bw, bh), radius=bh / 2 * s, fill=STAND)

    sx, sy, sw, sh, sr = spec["screen"]
    screen = Image.new("RGBA", (px, px), (0, 0, 0, 0))
    top, bottom = sy * s, (sy + sh) * s
    sd = ImageDraw.Draw(screen)
    for y in range(int(top), int(math.ceil(bottom))):
        sd.line([(0, y), (px, y)], fill=sky_color((y - top) / (bottom - top)))

    cx, cy, arm, thick = spec["star"]
    if spec["glow"]:
        glow = Image.new("RGBA", (px, px), (0, 0, 0, 0))
        r = spec["glow"] * s
        ImageDraw.Draw(glow).ellipse([cx * s - r, cy * s - r, cx * s + r, cy * s + r], fill=STAR + (90,))
        screen.alpha_composite(glow.filter(ImageFilter.GaussianBlur(r / 2.5)))
    sd = ImageDraw.Draw(screen)
    if spec["ridges"] > 1:
        sd.polygon([(x * s, y * s) for x, y in ridge_points(back_ridge, sx, sx + sw, sy + sh)], fill=BACK_RIDGE)
    sd.polygon([(x * s, y * s) for x, y in ridge_points(front_ridge, sx, sx + sw, sy + sh)], fill=FRONT_RIDGE)
    for rx0, ry0, rx1, ry1 in star_rects(cx, cy, arm, thick):
        sd.rectangle([rx0 * s, ry0 * s, rx1 * s, ry1 * s], fill=STAR)

    mask = Image.new("L", (px, px), 0)
    ImageDraw.Draw(mask).rounded_rectangle(box(sx, sy, sw, sh), radius=sr * s, fill=255)
    screen.putalpha(mask)
    img.alpha_composite(screen)
    return img.resize((size, size), Image.LANCZOS)


def svg():
    spec = FULL
    sx, sy, sw, sh, sr = spec["screen"]
    stops = "".join(f'<stop offset="{t}" stop-color="#{r:02x}{g:02x}{b:02x}"/>' for t, (r, g, b) in SKY)
    hexc = lambda c: "#%02x%02x%02x" % c
    poly = lambda fn: " ".join(f"{x:.2f},{y:.2f}" for x, y in ridge_points(fn, sx, sx + sw, sy + sh, 24))
    cx, cy, arm, thick = spec["star"]
    nx, ny, nw, nh = spec["neck"]
    bx, by, bw, bh = spec["base"]
    return (
        '<svg class="brand__mark" viewBox="0 0 32 32" aria-hidden="true">'
        f'<defs><linearGradient id="bm-sky" x1="0" y1="{sy}" x2="0" y2="{sy + sh}" gradientUnits="userSpaceOnUse">{stops}</linearGradient>'
        f'<radialGradient id="bm-glow"><stop offset="0" stop-color="{hexc(STAR)}" stop-opacity=".45"/>'
        f'<stop offset="1" stop-color="{hexc(STAR)}" stop-opacity="0"/></radialGradient>'
        f'<clipPath id="bm-screen"><rect x="{sx}" y="{sy}" width="{sw}" height="{sh}" rx="{sr}"/></clipPath></defs>'
        f'<rect x="{nx}" y="{ny}" width="{nw}" height="{nh}" fill="{hexc(STAND)}"/>'
        f'<rect x="{bx}" y="{by}" width="{bw}" height="{bh}" rx="{bh / 2}" fill="{hexc(STAND)}"/>'
        '<g clip-path="url(#bm-screen)">'
        f'<rect x="{sx}" y="{sy}" width="{sw}" height="{sh}" fill="url(#bm-sky)"/>'
        f'<circle cx="{cx}" cy="{cy}" r="{spec["glow"] * 1.3}" fill="url(#bm-glow)"/>'
        f'<polygon points="{poly(back_ridge)}" fill="{hexc(BACK_RIDGE)}"/>'
        f'<polygon points="{poly(front_ridge)}" fill="{hexc(FRONT_RIDGE)}"/>'
        f'<path d="M{cx - arm} {cy - thick / 2}h{2 * arm}v{thick}h{-2 * arm}zM{cx - thick / 2} {cy - arm}h{thick}v{2 * arm}h{-thick}z" fill="{hexc(STAR)}"/>'
        "</g></svg>"
    )


def main():
    sizes = [16, 20, 24, 32, 40, 48, 64, 256]
    pngs = []
    for size in sizes:
        buf = io.BytesIO()
        render(size).save(buf, "PNG")
        pngs.append(buf.getvalue())
    header = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries = b""
    for size, data in zip(sizes, pngs):
        entries += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    with open(os.path.join(HERE, "icon.ico"), "wb") as f:
        f.write(header + entries + b"".join(pngs))
    with open(os.path.join(HERE, "icon-256.png"), "wb") as f:
        f.write(pngs[-1])
    with open(os.path.join(HERE, "logo.svg"), "w", encoding="utf-8") as f:
        f.write(svg().replace('class="brand__mark" ', 'xmlns="http://www.w3.org/2000/svg" ') + "\n")
    page = os.path.join(HERE, "..", "ui", "index.html")
    with open(page, encoding="utf-8") as f:
        html = f.read()
    html = re.sub(r'<svg class="brand__mark".*?</svg>', lambda _: svg(), html, count=1, flags=re.S)
    with open(page, "w", encoding="utf-8") as f:
        f.write(html)
    print("wrote icon.ico, icon-256.png, logo.svg and the header logo in ui/index.html")


if __name__ == "__main__":
    main()
