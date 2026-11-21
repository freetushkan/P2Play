# P2Play icon: a peer node network with play in the middle, flat as pPlay's. Writes icon512.png
# (PS4/PS5 icon0.png, Android), icon256.jpg (Switch), icon192.png, banner.png (Android TV).
#   python3 tools/make-icon.py <dir>   (Noto Sans CJK Black)
import math, sys
from PIL import Image, ImageDraw, ImageFont

S = 2048  # drawn large, scaled down
BG = (242, 113, 28)        # orange
EMBLEM = (36, 40, 66)      # dark indigo
LIGHT = (246, 241, 232)    # cream
INK = (17, 17, 20)
FONT = "/usr/share/fonts/opentype/noto/NotoSansCJK-Black.ttc"


def emblem(draw, cx, cy, r, scale):
    # satellites (peers) linked to the node
    sats = []
    for ang in (-150, -30, 90):
        a = math.radians(ang)
        sats.append((cx + math.cos(a) * r * 1.58, cy + math.sin(a) * r * 1.58))
    w = int(r * 0.17)
    for (sx, sy) in sats:
        draw.line([(cx, cy), (sx, sy)], fill=EMBLEM, width=w)
    # ring between the satellites
    for i in range(3):
        draw.line([sats[i], sats[(i + 1) % 3]], fill=EMBLEM, width=int(w * 0.55))
    for (sx, sy) in sats:
        sr = r * 0.36
        draw.ellipse([sx - sr, sy - sr, sx + sr, sy + sr], fill=EMBLEM)
        ir = sr * 0.42
        draw.ellipse([sx - ir, sy - ir, sx + ir, sy + ir], fill=LIGHT)
    # the node with play
    draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=EMBLEM)
    t = r * 0.52
    ox = t * 0.12  # the triangle's visual centre
    draw.polygon([(cx - t * 0.62 + ox, cy - t), (cx - t * 0.62 + ox, cy + t), (cx + t * 1.0 + ox, cy)], fill=LIGHT)


def wordmark(text, height):
    font = ImageFont.truetype(FONT, height)
    box = font.getbbox(text)
    w, h = box[2] - box[0], box[3] - box[1]
    layer = Image.new("RGBA", (int(w * 1.4), int(h * 1.5)), (0, 0, 0, 0))
    ImageDraw.Draw(layer).text((int(w * 0.15) - box[0], int(h * 0.2) - box[1]), text, font=font, fill=INK)
    shear = 0.22
    layer = layer.transform(layer.size, Image.AFFINE, (1, shear, -shear * layer.size[1] * 0.6, 0, 1, 0),
                            resample=Image.BICUBIC)
    return layer.crop(layer.getbbox())


def square(size):
    im = Image.new("RGBA", (S, S), BG + (255,))
    d = ImageDraw.Draw(im)
    emblem(d, S * 0.5, S * 0.37, S * 0.145, S)
    wm = wordmark("P2Play", int(S * 0.2))
    wm = wm.resize((int(S * 0.58), int(S * 0.58 * wm.size[1] / wm.size[0])), Image.LANCZOS)
    im.alpha_composite(wm, (int((S - wm.size[0]) / 2), int(S * 0.75)))
    return im.resize((size, size), Image.LANCZOS)


def banner(w, h):
    k = 4
    im = Image.new("RGBA", (w * k, h * k), BG + (255,))
    d = ImageDraw.Draw(im)
    emblem(d, w * k * 0.24, h * k * 0.46, h * k * 0.17, h * k)
    wm = wordmark("P2Play", int(h * k * 0.3))
    ww = int(w * k * 0.46)
    wm = wm.resize((ww, int(ww * wm.size[1] / wm.size[0])), Image.LANCZOS)
    im.alpha_composite(wm, (int(w * k * 0.47), int((h * k - wm.size[1]) / 2)))
    return im.resize((w, h), Image.LANCZOS)


out = sys.argv[1]
square(512).save(out + "/icon512.png")
square(256).convert("RGB").save(out + "/icon256.jpg", quality=95)
square(192).save(out + "/icon192.png")
banner(320, 180).save(out + "/banner.png")
