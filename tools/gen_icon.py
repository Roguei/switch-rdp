"""Generate the NRO icon (assets/icon.jpg).

A 256x256 JPEG, because that is what the NRO asset section expects
(NroAssetHeader.icon).  Drawn from scratch so the icon can be rebuilt on any
machine: dark rounded tile, a monitor showing a remote desktop with the "RDP"
wordmark and the pointer the client draws over the stream.
"""

import os

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), "assets", "icon.jpg")

SIZE = 256
BG_TOP = (13, 22, 36)
BG_BOT = (26, 44, 70)
SCREEN_TOP = (32, 74, 124)
SCREEN_BOT = (16, 40, 72)
FRAME = (92, 176, 240)
ACCENT = (120, 214, 255)

FONT_BOLD = r"C:\Windows\Fonts\segoeuib.ttf"


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def vgradient(img, box, top, bot):
    x0, y0, x1, y1 = box
    d = ImageDraw.Draw(img)
    h = max(1, y1 - y0)
    for y in range(y0, y1):
        d.line([(x0, y), (x1, y)], fill=lerp(top, bot, (y - y0) / h))


def rounded_mask(size, radius):
    m = Image.new("L", (size, size), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, size - 1, size - 1], radius=radius, fill=255)
    return m


def main():
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    base = Image.new("RGB", (SIZE, SIZE), BG_TOP)
    vgradient(base, (0, 0, SIZE, SIZE), BG_TOP, BG_BOT)

    # --- monitor -----------------------------------------------------------------
    # Screen body, frame and a hint of the stand, all centred on the tile.
    screen = (44, 52, 212, 168)
    d = ImageDraw.Draw(base)
    d.rounded_rectangle([screen[0] - 6, screen[1] - 6, screen[2] + 6, screen[3] + 6],
                        radius=14, fill=FRAME)
    d.rounded_rectangle([screen[0] - 2, screen[1] - 2, screen[2] + 2, screen[3] + 2],
                        radius=11, fill=(10, 16, 26))
    vgradient(base, (screen[0] + 2, screen[1] + 2, screen[2] - 1, screen[3] - 1),
              SCREEN_TOP, SCREEN_BOT)
    # stand
    d.rounded_rectangle([120, 168, 136, 190], radius=4, fill=FRAME)
    d.rounded_rectangle([96, 188, 160, 200], radius=6, fill=FRAME)

    # desktop: two window bars, so it reads as "a remote desktop", not a TV
    d.rounded_rectangle([52, 60, 204, 72], radius=4, fill=(44, 92, 148))
    for i, col in enumerate(((255, 120, 120), (255, 208, 120), (140, 230, 160))):
        d.ellipse([58 + i * 12, 63, 64 + i * 12, 69], fill=col)
    d.rounded_rectangle([52, 80, 140, 86], radius=3, fill=(56, 108, 168))
    d.rounded_rectangle([52, 92, 120, 98], radius=3, fill=(44, 92, 148))

    # "RDP" wordmark inside the screen
    try:
        font = ImageFont.truetype(FONT_BOLD, 52)
    except OSError:
        font = ImageFont.load_default()
    text = "RDP"
    tw = d.textlength(text, font=font)
    d.text((120 - tw / 2, 100), text, font=font, fill=ACCENT)

    # pointer, drawn like the client's own cursor (white with a black outline),
    # kept in the lower right corner so it does not cover the wordmark
    cur = [(180, 122), (180, 152), (187, 146), (192, 157),
           (198, 155), (193, 144), (202, 144)]
    d.polygon(cur, fill=(255, 255, 255), outline=(0, 0, 0))

    # round the tile corners and save as JPEG
    out = Image.new("RGB", (SIZE, SIZE), BG_BOT)
    out.paste(base, (0, 0), rounded_mask(SIZE, 44))
    out.save(OUT, "JPEG", quality=92, subsampling=0)
    print("wrote %s (%d bytes)" % (OUT, os.path.getsize(OUT)))


if __name__ == "__main__":
    main()
