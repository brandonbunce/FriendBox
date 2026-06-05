#!/usr/bin/env python3
"""Generate src/lt_glyph_data.cpp — 32x32 UCG glyph dot-matrices for the LT7680
character engine. Glyphs are 32 wide x 32 tall, packed MSB-first (bit7 = leftmost
column), 4 bytes/row, 128 bytes/glyph. They are 1-bit masks: the LT7680 recolors
them at draw time, so design in black & white only.

32x32 are "full width" UCG glyphs (datasheet 8.2.6): the character code is
0x8000 + index, and the engine reads glyph `index` from CGRAM_STR + index*128.
The GlyphCode enum therefore starts at 0x8000 (handled in include/lt_assets.hpp).

==========================================================================
HOW TO MAKE YOUR OWN GLYPH
==========================================================================

Two authoring styles (mix freely in the GLYPHS list below):

  1. ASCII ART (easiest — just draw it). A 32-line string, each line up to 32
     chars. '#','X','x','@','*','o' = pixel ON; space / '.' / anything else = OFF.
     See the HEART example. Leave ~2px margin so the x2-x4 `enlarge` doesn't clip.

  2. DRAW FUNCTION (good for crisp geometric icons). Build a grid() with the
     line / hline / vline / rect_fill / rect_outline helpers (coords are 0..31).

STEPS:
  1. Add your glyph to the GLYPHS list (ASCII art via art("..."), or a function).
  2. Add a matching name to `enum GlyphCode` in include/lt_assets.hpp, IN THE
     SAME ORDER as GLYPHS (insert before GLYPH__END). Order is load-bearing:
     glyph i == character code (0x8000+i) == CGRAM byte offset i*128.
  3. Regenerate:   python3 tools/make_glyphs.py     (rewrites src/lt_glyph_data.cpp)
  4. Bump LT_ASSET_VERSION in include/lt_assets.hpp. REQUIRED — the glyph blob is
     cached in the LT7680's flash and only reprogrammed when the version changes.
  5. pio run + flash.

USE IT (firmware):
     ui::glyphCentered(GLYPH_STAR, x, y, w, h, fg565);   // centered in a box
     ltDrawGlyph(GLYPH_STAR, x, y, fg565);               // raw top-left at (x,y)
  Sizes: native 32x32; `enlarge` 1..4 scales up (x4 = 128x128, blocky).

TIP: verify packing without flashing — this prints glyph i back as ASCII:
     python3 -c "import re; d=open('src/lt_glyph_data.cpp').read(); \\
       n=[int(x,16) for x in re.findall(r'0x([0-9A-Fa-f]{2})',d)]; i=0; \\
       [print(''.join(format(n[i*128+r*4+c],'08b') for c in range(4)) \\
         .replace('0','.').replace('1','#')) for r in range(32)]"
"""
import os

W, H = 32, 32

def blank():
    return [[0] * W for _ in range(H)]

def art(rows):
    """Turn a 32-line ASCII string (32 cols each) into a glyph grid.
    On pixels: any of '#','X','x','@','*','o'. Off: space, '.', or anything else."""
    lines = rows.split("\n")
    if lines and lines[0] == "":          # drop leading newline from triple-quote
        lines = lines[1:]
    g = blank()
    on = set("#X@*xo")
    for y in range(min(H, len(lines))):
        row = lines[y]
        for x in range(min(W, len(row))):
            if row[x] in on:
                g[y][x] = 1
    return g

def px(g, x, y):
    if 0 <= x < W and 0 <= y < H:
        g[y][x] = 1

def hline(g, x0, x1, y, t=1):
    for x in range(x0, x1 + 1):
        for dy in range(t):
            px(g, x, y + dy)

def vline(g, x, y0, y1, t=1):
    for y in range(y0, y1 + 1):
        for dx in range(t):
            px(g, x + dx, y)

def rect_fill(g, x0, y0, x1, y1):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            px(g, x, y)

def rect_outline(g, x0, y0, x1, y1, t=2):
    for i in range(t):
        hline(g, x0, x1, y0 + i); hline(g, x0, x1, y1 - i)
        vline(g, x0 + i, y0, y1); vline(g, x1 - i, y0, y1)

def line(g, x0, y0, x1, y1, t=2):
    dx = abs(x1 - x0); dy = -abs(y1 - y0)
    sx = 1 if x0 < x1 else -1; sy = 1 if y0 < y1 else -1
    err = dx + dy; x, y = x0, y0
    while True:
        for ax in range(t):
            for ay in range(t):
                px(g, x + ax, y + ay)
        if x == x1 and y == y1: break
        e2 = 2 * err
        if e2 >= dy: err += dy; x += sx
        if e2 <= dx: err += dx; y += sy

# --- geometric glyphs, drawn in 32x32 space (cols/rows 0..31) ---------------

def g_play():
    g = blank()
    top, bot, left = 6, 26, 9
    h = bot - top
    for y in range(top, bot + 1):
        frac = (y - top) / h
        xend = int(left + 16 * (1 - abs(2 * frac - 1)))
        hline(g, left, xend, y)
    return g

def g_pause():
    g = blank()
    rect_fill(g, 7, 6, 13, 26)
    rect_fill(g, 19, 6, 25, 26)
    return g

def g_stop():
    g = blank()
    rect_fill(g, 6, 6, 25, 25)
    return g

def g_loop():
    g = blank()
    rect_outline(g, 6, 10, 25, 23, t=2)
    line(g, 25, 10, 19, 4, t=2); line(g, 25, 10, 19, 16, t=2)   # top-right arrowhead
    line(g, 6, 23, 12, 17, t=2); line(g, 6, 23, 12, 29, t=2)    # bottom-left arrowhead
    return g

def g_plus():
    g = blank()
    rect_fill(g, 13, 5, 18, 27)
    rect_fill(g, 5, 13, 27, 18)
    return g

def g_minus():
    g = blank()
    rect_fill(g, 5, 14, 27, 18)
    return g

def g_chev_l():
    g = blank()
    line(g, 20, 4, 9, 16, t=4); line(g, 9, 16, 20, 28, t=4)
    return g

def g_chev_r():
    g = blank()
    line(g, 11, 4, 22, 16, t=4); line(g, 22, 16, 11, 28, t=4)
    return g

def g_check():
    g = blank()
    line(g, 5, 17, 13, 25, t=4); line(g, 13, 25, 27, 7, t=4)
    return g

def g_x():
    g = blank()
    line(g, 6, 6, 25, 25, t=4); line(g, 25, 6, 6, 25, t=4)
    return g

def g_send():
    g = blank()
    line(g, 4, 27, 28, 5, t=3)      # spine (arrow up-right)
    line(g, 28, 5, 16, 8, t=3)      # upper barb
    line(g, 28, 5, 25, 17, t=3)     # lower barb
    line(g, 4, 27, 18, 12, t=2)     # fold
    return g

def g_folder():
    g = blank()
    rect_outline(g, 4, 11, 27, 26, t=2)
    rect_fill(g, 4, 7, 14, 12)      # tab
    return g

# Example ASCII-art glyph (32x32). Draw with '#' (on), '.'/space (off).
HEART = art("""
................................
................................
................................
.....######........######.......
...##########....##########.....
..############..############....
.#############################..
.#############################..
.#############################..
.#############################..
.#############################..
.#############################..
.#############################..
..###########################...
..###########################...
...#########################....
....#######################.....
.....#####################......
......###################.......
.......#################........
........###############.........
.........#############..........
..........###########...........
...........#########............
............#######.............
.............#####..............
..............###...............
...............#................
................................
................................
................................
................................
""")

# Order MUST match enum GlyphCode in include/lt_assets.hpp.
GLYPHS = [
    ("PLAY", g_play), ("PAUSE", g_pause), ("STOP", g_stop), ("LOOP", g_loop),
    ("PLUS", g_plus), ("MINUS", g_minus), ("CHEV_L", g_chev_l), ("CHEV_R", g_chev_r),
    ("CHECK", g_check), ("X", g_x), ("SEND", g_send), ("FOLDER", g_folder),
    ("HEART", HEART),
]

def pack(g):
    out = []
    for y in range(H):
        for bx in range(0, W, 8):          # 4 bytes per 32-px row
            b = 0
            for x in range(8):
                if g[y][bx + x]: b |= (1 << (7 - x))
            out.append(b)
    return out

def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(here, "src", "lt_glyph_data.cpp")
    blobs = []
    for name, g in GLYPHS:
        grid = g() if callable(g) else g    # function -> call it; art() -> already a grid
        blobs.append((name, pack(grid)))
    total = sum(len(b) for _, b in blobs)
    with open(out, "w") as f:
        f.write("// AUTO-GENERATED by tools/make_glyphs.py - do not edit by hand.\n")
        f.write("// 32x32 UCG glyph dot-matrices, 4 bytes/row, MSB=leftmost column.\n")
        f.write('#include "lt_assets.hpp"\n\n')
        f.write("const uint8_t lt_glyph_data[] = {\n")
        for name, b in blobs:
            f.write(f"    // {name}\n    ")
            f.write(", ".join(f"0x{v:02X}" for v in b))
            f.write(",\n")
        f.write("};\n")
        f.write(f"const uint32_t lt_glyph_data_len = {total};\n")
    print(f"wrote {out} ({total} bytes, {len(blobs)} glyphs)")

if __name__ == "__main__":
    main()
