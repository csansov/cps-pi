#!/usr/bin/env python3
"""cps-pi: composes dist/sd/cpspi/overlay.png (the bezel shown around the
game picture) from art/overlay-source.jpg, a 1920x1080 bezel with a white
4:3 window from x 241 to 1679: its carbon texture, its two logo panels and
its gold border are laid out again around cps-pi's picture, which at 1080p
is 1194x896 at (360, 92) (integer 4x vertically, 4:3 horizontally).

    python tools/make_overlay.py [source.jpg] [out.png]

The kernel shows any 8 bit RGB/RGBA PNG the size of the display found at
cpspi/overlay.png; only the area outside the picture is used.
"""
import sys
from PIL import Image, ImageDraw

src_path = sys.argv[1] if len(sys.argv) > 1 else "art/overlay-source.jpg"
out_path = sys.argv[2] if len(sys.argv) > 2 else "dist/sd/cpspi/overlay.png"

W, H = 1920, 1080
PX0, PX1, PY0, PY1 = 360, 1554, 92, 988		# the game picture on screen
GAP, GOLD = 4, 8					# dark gap, then the gold frame

src = Image.open(src_path).convert("RGB")
if src.size != (W, H):
    sys.exit("the source bezel must be %dx%d" % (W, H))

# the carbon texture repeats every 8 pixels; the panels are darker at the
# screen edge and brighten towards the frame, so the texture that continues
# them (from each panel's inner edge to the frame, and the bands above and
# below the picture) is one period of the weave with a brightness ramp that
# meets the panel's inner columns and ends a little brighter at the frame
tile = src.crop((184, 400, 184 + 8, 400 + 8)).convert("RGB")
tile_mean = sum(tile.convert("L").tobytes()) / 64.0


def mean_cols(x0, x1):
    g = src.convert("L")
    return sum(g.getpixel((x, y)) for x in range(x0, x1) for y in range(300, 800)) / float((x1 - x0) * 500)


inner_l = mean_cols(222, 230)		# the left panel next to its inner edge
inner_r = mean_cols(1690, 1698)
at_frame = max(inner_l, inner_r) + 6
out = Image.new("RGB", (W, H))
px = out.load()
tp = tile.load()


def fill(x0, x1, y0, y1, bright):
    """texture over the rectangle, with the brightness bright(x) per column"""
    for x in range(x0, x1):
        gain = bright(x) / tile_mean
        for y in range(y0, y1):
            r, g, b = tp[x % 8, y % 8]
            px[x, y] = (min(255, int(r * gain + 0.5)), min(255, int(g * gain + 0.5)), min(255, int(b * gain + 0.5)))


FX0, FX1 = PX0 - GAP - GOLD, PX1 + GAP + GOLD		# the frame's outer edges
fill(230, FX0, 0, H, lambda x: inner_l + (at_frame - inner_l) * (x - 230) / float(FX0 - 230))
fill(FX1, 1690, 0, H, lambda x: at_frame + (inner_r - at_frame) * (x - FX1) / float(1690 - FX1))
fill(FX0, FX1, 0, H, lambda x: at_frame)

# the logo panels where they are in the source, darkest at the screen edge
out.paste(src.crop((0, 0, 230, H)), (0, 0))
out.paste(src.crop((1690, 0, 1920, H)), (1690, 0))

# the frame: gold outside, a dark line inside, as in the source
gold = src.getpixel((234, 540))
dark = src.getpixel((239, 540))
d = ImageDraw.Draw(out)
d.rectangle((PX0 - GAP - GOLD, PY0 - GAP - GOLD, PX1 + GAP + GOLD - 1, PY1 + GAP + GOLD - 1), fill=gold)
d.rectangle((PX0 - GAP, PY0 - GAP, PX1 + GAP - 1, PY1 + GAP - 1), fill=dark)
d.rectangle((PX0, PY0, PX1 - 1, PY1 - 1), fill=(0, 0, 0))

out.save(out_path, optimize=True)
print("%s: %dx%d, picture window (%d,%d)-(%d,%d)" % (out_path, W, H, PX0, PY0, PX1, PY1))
