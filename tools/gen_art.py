#!/usr/bin/env python3
"""
Gera a pixel-art (1 bit) dos ecrãs OLED e escreve src/display/art.c.

    python3 tools/gen_art.py            # gera art.c
    python3 tools/gen_art.py --preview  # também gera PNGs em tools/preview/

Todos os desenhos são originais. Convenção: 1 = pixel aceso.
"""
import os
import sys
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_C = os.path.join(ROOT, "src", "display", "art.c")
PREVIEW = os.path.join(ROOT, "tools", "preview")

ASSETS = {}  # name -> PIL image (mode "1")


def new(w, h):
    return Image.new("1", (w, h), 0)


def add(name, img):
    ASSETS[name] = img


def from_rows(rows):
    h = len(rows)
    w = max(len(r) for r in rows)
    img = new(w, h)
    for y, r in enumerate(rows):
        for x, c in enumerate(r):
            if c in "#X":
                img.putpixel((x, y), 1)
    return img


# --------------------------------------------------------------------------
# Ícones das layers (32x32, 2 frames cada)
# --------------------------------------------------------------------------

def icon_keyboard(pressed):
    img = new(32, 32)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([1, 7, 30, 25], radius=3, outline=1)
    # 3 filas de teclas
    for row, (y, n, x0) in enumerate([(10, 7, 4), (14, 7, 5), (18, 6, 6)]):
        for i in range(n):
            x = x0 + i * 4
            if pressed and row == 1 and i == 3:
                d.rectangle([x, y + 1, x + 2, y + 3], fill=1)
            else:
                d.rectangle([x, y, x + 2, y + 2], outline=1)
    # barra de espaço
    if pressed:
        d.rectangle([9, 22, 22, 23], fill=1)
    else:
        d.rectangle([9, 22, 22, 22], fill=1)
    return img


DIGITS = {
    "1": ["..#.", ".##.", "..#.", "..#.", "..#.", "..#.", ".###"],
    "2": [".##.", "#..#", "...#", "..#.", ".#..", "#...", "####"],
    "3": ["###.", "...#", "...#", ".##.", "...#", "...#", "###."],
    "4": ["..#.", ".##.", "#.#.", "####", "..#.", "..#.", "..#."],
    "5": ["####", "#...", "###.", "...#", "...#", "#..#", ".##."],
    "6": [".##.", "#...", "###.", "#..#", "#..#", "#..#", ".##."],
    "#": [".#.#", ".#.#", "####", ".#.#", "####", ".#.#", ".#.#"],
    "&": [".##.", "#..#", ".##.", "#.#.", "#..#", "#.##", ".##."],
    "%": ["#..#", "#..#", "..#.", ".#..", "#...", "#..#", "#..#"],
    "!": [".#..", ".#..", ".#..", ".#..", ".#..", "....", ".#.."],
    "@": [".##.", "#..#", "#.##", "#.##", "#.#.", "#...", ".###"],
    "?": [".##.", "#..#", "...#", "..#.", ".#..", "....", ".#.."],
    "$": ["..#.", ".###", "#.#.", ".##.", "..##", "###.", "..#."],
}


def big_glyph(img, ch, x0, y0, scale=2):
    rows = DIGITS[ch]
    for y, r in enumerate(rows):
        for x, c in enumerate(r):
            if c == "#":
                for dy in range(scale):
                    for dx in range(scale):
                        img.putpixel((x0 + x * scale + dx, y0 + y * scale + dy), 1)


def icon_text(chars, frame, box=True):
    img = new(32, 32)
    d = ImageDraw.Draw(img)
    if box:
        d.rounded_rectangle([0, 3, 31, 28], radius=4, outline=1)
    xs = [3, 12, 21] if len(chars) == 3 else [7, 17]
    for i, ch in enumerate(chars):
        y = 9 + (-1 if (frame and i % 2 == 0) else 0) + (1 if (frame and i % 2 == 1) else 0)
        big_glyph(img, ch, xs[i], y)
    return img


ARROW = [
    "#...........",
    "##..........",
    "#X#.........",
    "#XX#........",
    "#XXX#.......",
    "#XXXX#......",
    "#XXXXX#.....",
    "#XXXXXX#....",
    "#XXXXXXX#...",
    "#XXXXXXXX#..",
    "#XXXXX#####.",
    "#XX#XX#.....",
    "#X#.#XX#....",
    "##..#XX#....",
    "#....#XX#...",
    ".....#XX#...",
    "......##....",
]


def icon_mouse(frame):
    img = new(32, 32)
    d = ImageDraw.Draw(img)
    # seta (contorno + interior vazio para ficar legível)
    for y, r in enumerate(ARROW):
        for x, c in enumerate(r):
            if c == "#":
                img.putpixel((9 + x, 6 + y), 1)
    # "ondas" de clique no frame 1
    if frame:
        for rr in (4, 7):
            d.arc([9 - rr, 6 - rr, 9 + rr, 6 + rr], 180, 270, fill=1)
    # pequeno trackpad em baixo à direita
    d.rounded_rectangle([20, 22, 30, 30], radius=1, outline=1)
    d.point((25 if frame else 23, 26), fill=1)
    return img


for f in (0, 1):
    add(f"icon_base_{f}", icon_keyboard(f))
    add(f"icon_lower_{f}", icon_text("123" if not f else "456", f))
    add(f"icon_raise_{f}", icon_text("#!" if not f else "$%", f))
    add(f"icon_mouse_{f}", icon_mouse(f))
    add(f"icon_other_{f}", icon_text("@?", f))

# --------------------------------------------------------------------------
# Ícones pequenos 8x8
# --------------------------------------------------------------------------
add("sym_bt", from_rows([
    "..#.....",
    "..##....",
    "#.#.#...",
    ".###....",
    ".###....",
    "#.#.#...",
    "..##....",
    "..#.....",
]))
add("sym_usb", from_rows([
    "...#....",
    "..###...",
    "...#.#..",
    "#..#.#..",
    "#..#.#..",
    ".#.##...",
    "..##....",
    "...#....",
]))
add("sym_ok", from_rows([
    "........",
    ".......#",
    "......##",
    "#....##.",
    "##..##..",
    ".####...",
    "..##....",
    "........",
]))
add("sym_off", from_rows([
    "........",
    "#.....#.",
    ".#...#..",
    "..#.#...",
    "...#....",
    "..#.#...",
    ".#...#..",
    "#.....#.",
]))
add("sym_open", from_rows([   # perfil livre (por emparelhar)
    "........",
    "..###...",
    ".#...#..",
    ".....#..",
    "....#...",
    "...#....",
    "........",
    "...#....",
]))
add("sym_link", from_rows([   # ligado à central
    "........",
    ".##..##.",
    "#..##..#",
    "#.#..#.#",
    "#.#..#.#",
    "#..##..#",
    ".##..##.",
    "........",
]))
add("sym_scroll", from_rows([
    "...#....",
    "..###...",
    ".#.#.#..",
    "...#....",
    "...#....",
    ".#.#.#..",
    "..###...",
    "...#....",
]))
add("sym_tap", from_rows([
    "..###...",
    ".#...#..",
    "#..#..#.",
    "#.###.#.",
    "#..#..#.",
    ".#...#..",
    "..###...",
    "........",
]))
add("sym_ptr", from_rows([
    "#.......",
    "##......",
    "#X#.....",
    "#XX#....",
    "#XXX#...",
    "#XX##...",
    "#..#....",
    "....#...",
]))
add("sym_zz", from_rows([
    "###.....",
    "..#.....",
    ".#......",
    "###.###.",
    ".....#..",
    "....#...",
    "....###.",
    "........",
]))

# --------------------------------------------------------------------------
# Bateria 14x7: 0..5 níveis + a carregar
# --------------------------------------------------------------------------

def battery(level, charging=False):
    img = new(14, 7)
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, 11, 6], outline=1)
    d.rectangle([12, 2, 13, 4], fill=1)
    if charging:
        for x, y in [(6, 1), (5, 2), (4, 3), (5, 3), (6, 3), (7, 3), (6, 4), (5, 5)]:
            img.putpixel((x, y), 1)
    else:
        for i in range(level):
            d.rectangle([2 + i * 2, 2, 2 + i * 2, 4], fill=1)
    return img


for lv in range(6):
    add(f"bat_{lv}", battery(lv))
add("bat_chg", battery(0, True))

# --------------------------------------------------------------------------
# Mascote original "Pixo" (32x32): robô-bolha com antena
# --------------------------------------------------------------------------

def pixo(eyes="open", look=0, arms="down", mouth="smile", bounce=0, zz=False, antenna=0):
    img = new(32, 32)
    d = ImageDraw.Draw(img)
    oy = bounce
    # antena
    ax = 16 + antenna
    d.line([16, 8 + oy, ax, 4 + oy], fill=1)
    d.ellipse([ax - 2, 1 + oy, ax + 2, 5 + oy], outline=1)
    # corpo
    d.rounded_rectangle([6, 8 + oy, 25, 26 + oy], radius=7, outline=1)
    # pés
    d.line([10, 27 + oy, 13, 27 + oy], fill=1)
    d.line([18, 27 + oy, 21, 27 + oy], fill=1)
    # olhos
    ey = 14 + oy
    for ex in (11, 19):
        if eyes == "open":
            d.rectangle([ex + look - 1, ey - 1, ex + look + 1, ey + 2], fill=1)
        elif eyes == "closed":
            d.line([ex - 1, ey + 1, ex + 2, ey + 1], fill=1)
        elif eyes == "happy":
            d.line([ex - 1, ey + 1, ex, ey], fill=1)
            d.line([ex, ey, ex + 1, ey + 1], fill=1)
        elif eyes == "wide":
            d.ellipse([ex + look - 2, ey - 2, ex + look + 2, ey + 2], outline=1)
            img.putpixel((ex + look, ey), 1)
    # boca
    my = 20 + oy
    if mouth == "smile":
        d.line([13, my, 14, my + 1], fill=1)
        d.line([15, my + 1, 16, my + 1], fill=1)
        d.line([17, my + 1, 18, my], fill=1)
    elif mouth == "open":
        d.rectangle([14, my, 17, my + 2], outline=1)
    elif mouth == "flat":
        d.line([14, my + 1, 17, my + 1], fill=1)
    elif mouth == "o":
        d.rectangle([15, my, 16, my + 1], outline=1)
    # braços
    if arms == "down":
        d.line([5, 17 + oy, 3, 21 + oy], fill=1)
        d.line([26, 17 + oy, 28, 21 + oy], fill=1)
    elif arms == "up":
        d.line([5, 16 + oy, 2, 12 + oy], fill=1)
        d.line([26, 16 + oy, 29, 12 + oy], fill=1)
    elif arms == "left_up":
        d.line([5, 16 + oy, 2, 12 + oy], fill=1)
        d.line([26, 17 + oy, 28, 21 + oy], fill=1)
    elif arms == "right_up":
        d.line([5, 17 + oy, 3, 21 + oy], fill=1)
        d.line([26, 16 + oy, 29, 12 + oy], fill=1)
    elif arms == "point":
        d.line([26, 17 + oy, 31, 17 + oy], fill=1)
        d.line([5, 17 + oy, 3, 21 + oy], fill=1)
    if zz:
        for x, y in [(26, 1), (27, 1), (28, 1), (27, 2), (26, 3), (27, 3), (28, 3)]:
            img.putpixel((x, y), 1)
    return img


MASCOT = {
    "idle_0": dict(eyes="open"),
    "idle_1": dict(eyes="open", antenna=1),
    "idle_2": dict(eyes="closed", antenna=1),       # pestanejar
    "type_0": dict(eyes="happy", arms="left_up", mouth="open", bounce=1),
    "type_1": dict(eyes="happy", arms="right_up", mouth="open", bounce=0),
    "pad_0": dict(eyes="open", look=1, arms="point", mouth="o"),
    "pad_1": dict(eyes="open", look=2, arms="point", mouth="o", antenna=1),
    "scroll_0": dict(eyes="wide", look=0, arms="up", mouth="o", bounce=0),
    "scroll_1": dict(eyes="wide", look=0, arms="up", mouth="o", bounce=1),
    "sleep_0": dict(eyes="closed", mouth="flat", zz=True, bounce=1),
    "sleep_1": dict(eyes="closed", mouth="flat", zz=False, bounce=1, antenna=-1),
    "sad_0": dict(eyes="open", mouth="flat", arms="down"),
}
for k, v in MASCOT.items():
    add(f"pixo_{k}", pixo(**v))

# --------------------------------------------------------------------------
# Saída C (LVGL 9, LV_COLOR_FORMAT_I1)
# --------------------------------------------------------------------------

def to_i1_bytes(img):
    w, h = img.size
    stride = (w + 7) // 8
    out = []
    for y in range(h):
        for bx in range(stride):
            b = 0
            for bit in range(8):
                x = bx * 8 + bit
                if x < w and img.getpixel((x, y)):
                    b |= 0x80 >> bit
            out.append(b)
    return out, stride


def write_c():
    lines = [
        "/* GERADO por tools/gen_art.py — não editar à mão. */",
        "#include <lvgl.h>",
        "",
        "#ifndef LV_ATTRIBUTE_LARGE_CONST",
        "#define LV_ATTRIBUTE_LARGE_CONST",
        "#endif",
        "",
        "/* Paleta: índice 0 = fundo (branco no LVGL, aparece apagado no OLED invertido),",
        "   índice 1 = primeiro plano (preto no LVGL, aparece aceso). Igual ao tema mono do ZMK. */",
        "#define CTPS_PALETTE 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0xff",
        "",
    ]
    for name, img in ASSETS.items():
        data, stride = to_i1_bytes(img)
        w, h = img.size
        lines.append(f"static const LV_ATTRIBUTE_LARGE_CONST uint8_t {name}_map[] = {{")
        lines.append("    CTPS_PALETTE,")
        for i in range(0, len(data), 16):
            lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) + ",")
        lines.append("};")
        lines.append(f"const lv_image_dsc_t {name} = {{")
        lines.append("    .header.magic = LV_IMAGE_HEADER_MAGIC,")
        lines.append("    .header.cf = LV_COLOR_FORMAT_I1,")
        lines.append(f"    .header.w = {w},")
        lines.append(f"    .header.h = {h},")
        lines.append(f"    .header.stride = {stride},")
        lines.append(f"    .data_size = sizeof({name}_map),")
        lines.append(f"    .data = {name}_map,")
        lines.append("};")
        lines.append("")
    os.makedirs(os.path.dirname(OUT_C), exist_ok=True)
    with open(OUT_C, "w") as f:
        f.write("\n".join(lines))
    # header
    hdr = ["/* GERADO por tools/gen_art.py */", "#pragma once", "#include <lvgl.h>", ""]
    hdr += [f"extern const lv_image_dsc_t {n};" for n in ASSETS]
    with open(os.path.join(os.path.dirname(OUT_C), "art.h"), "w") as f:
        f.write("\n".join(hdr) + "\n")


def write_previews(scale=8):
    os.makedirs(PREVIEW, exist_ok=True)
    names = list(ASSETS)
    cols = 8
    cell = 36
    rows = (len(names) + cols - 1) // cols
    sheet = Image.new("L", (cols * cell, rows * cell), 40)
    for i, n in enumerate(names):
        img = ASSETS[n].convert("L").point(lambda p: 255 if p else 0)
        sheet.paste(img, ((i % cols) * cell + 2, (i // cols) * cell + 2))
    sheet = sheet.resize((sheet.width * 4, sheet.height * 4), Image.NEAREST)
    sheet.save(os.path.join(PREVIEW, "sheet.png"))


if __name__ == "__main__":
    write_c()
    if "--preview" in sys.argv:
        write_previews()
    print(f"{len(ASSETS)} imagens -> {OUT_C}")
