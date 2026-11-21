#!/usr/bin/env python3
"""src/app/screen/font.cpp: the on-screen log's font (DejaVu Sans Mono 20 px, ASCII, 12x24 masks)."""
from PIL import Image, ImageDraw, ImageFont

W, H = 12, 24
font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 20)
asc, desc = font.getmetrics()
data = b""
for c in range(32, 127):
    im = Image.new("L", (W, H), 0)
    ImageDraw.Draw(im).text((0, (H - (asc + desc)) // 2), chr(c), font=font, fill=255)
    data += bytes(im.getdata())
lines = ["    " + ",".join(str(b) for b in data[i:i + 24]) + "," for i in range(0, len(data), 24)]
with open("src/app/screen/font.cpp", "w") as f:
    f.write('#include "font.hpp"\n\nnamespace p2p {\n    const uint8_t kFont[] = {\n' + "\n".join(lines) + "\n    };\n}\n")
