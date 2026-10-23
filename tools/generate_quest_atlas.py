"""Regenerate the embedded atlas losslessly; requires Pillow only for this tool."""
from pathlib import Path
from PIL import Image

root = Path(__file__).resolve().parents[1]
image = Image.open(root / "assets/quest-icons.png").convert("RGB")
assert image.size == (320, 80)
runs = []
for color in image.get_flattened_data():
    if runs and runs[-1][1] == color and runs[-1][0] < 65535:
        runs[-1] = (runs[-1][0] + 1, color)
    else:
        runs.append((1, color))
header = """#pragma once
// Generated losslessly from assets/quest-icons.png. Do not hand edit.
#include <cstdint>
namespace encounters::questicons::atlas {
constexpr unsigned Width = 320, Height = 80;
struct Run { uint16_t count; uint8_t r, g, b; };
constexpr Run Pixels[] = {
"""
header += "".join("{%d,%d,%d,%d},\n" % (count, *color) for count, color in runs)
header += "};\n}\n"
(root / "src/QuestAtlas.h").write_text(header)
