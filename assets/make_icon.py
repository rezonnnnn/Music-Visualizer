"""Draws the app icon (four neon bars on a dark tile) and writes assets/icon.ico. Needs Pillow."""
from pathlib import Path
from PIL import Image, ImageDraw

S = 256
im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(im)
d.rounded_rectangle((8, 8, S - 8, S - 8), radius=48, fill=(11, 13, 20, 255))
heights = (0.42, 0.78, 0.56, 0.92)
bar, gap, left, base = 38, 14, 31, 208
for i, h in enumerate(heights):
    x = left + i * (bar + gap)
    top = base - int(150 * h)
    d.rectangle((x, top, x + bar, base), fill=(0, 240, 255, 255))
    d.rectangle((x, top - 20, x + bar, top - 12), outline=(0, 240, 255, 255), width=3)
im.save(Path(__file__).with_name("icon.ico"), sizes=[(16, 16), (20, 20), (24, 24), (32, 32), (48, 48), (64, 64), (256, 256)])
print("wrote icon.ico")
