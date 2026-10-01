#!/usr/bin/env python3
"""Pré-visualização APROXIMADA dos dois ecrãs OLED (as fontes reais são as do LVGL)."""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(__file__))
import gen_art as A  # noqa: E402

OUT = os.path.join(os.path.dirname(__file__), "preview")
BIG = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 13)
SMALL = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 8)


def paste(scr, name, x, y):
    scr.paste(A.ASSETS[name], (x, y), A.ASSETS[name])


def left(layer="BASE", icon="icon_base_0", bt="sym_bt", prof="1", link="sym_ok", bl=4, br=3, wpm=42):
    s = Image.new("1", (128, 32), 0)
    d = ImageDraw.Draw(s)
    paste(s, icon, 0, 0)
    d.text((36, -1), layer, font=BIG, fill=1)
    paste(s, bt, 36, 16)
    d.text((45, 15), prof, font=SMALL, fill=1)
    paste(s, link, 54, 16)
    d.text((36, 23), "L", font=SMALL, fill=1)
    paste(s, f"bat_{bl}", 44, 25)
    d.text((62, 23), "R", font=SMALL, fill=1)
    paste(s, f"bat_{br}", 71, 25)
    d.text((104, 15), "wpm", font=SMALL, fill=1)
    d.text((104, 23), f"{wpm:3d}", font=SMALL, fill=1)
    return s


def right(mascot="pixo_idle_0", dot=None, mode="IDLE", mimg="sym_ok", link=True, bat=4, pct="87%", click=False):
    s = Image.new("1", (128, 32), 0)
    d = ImageDraw.Draw(s)
    paste(s, mascot, 0, 0)
    w = 3 if click else 1
    d.rounded_rectangle([38, 2, 77, 29], radius=3, outline=1, width=w)
    if dot:
        x, y = dot
        d.ellipse([39 + x, 3 + y, 42 + x, 6 + y], fill=1)
    paste(s, mimg, 82, 0)
    d.text((92, -1), mode, font=SMALL, fill=1)
    paste(s, "sym_link" if link else "sym_off", 82, 12)
    d.text((92, 11), "LINK" if link else "----", font=SMALL, fill=1)
    paste(s, f"bat_{bat}", 82, 25)
    d.text((98, 23), pct, font=SMALL, fill=1)
    return s


def oled(img, scale=5):
    w, h = img.size
    big = Image.new("RGB", (w * scale + 16, h * scale + 16), (12, 12, 14))
    px = img.load()
    d = ImageDraw.Draw(big)
    for y in range(h):
        for x in range(w):
            if px[x, y]:
                x0, y0 = 8 + x * scale, 8 + y * scale
                d.rectangle([x0, y0, x0 + scale - 2, y0 + scale - 2], fill=(180, 230, 255))
    return big


def sheet(items, title_font=ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 18)):
    tiles = [(t, oled(i)) for t, i in items]
    W = max(t.width for _, t in tiles) + 40
    H = sum(t.height + 40 for _, t in tiles) + 20
    out = Image.new("RGB", (W, H), (245, 245, 247))
    d = ImageDraw.Draw(out)
    y = 10
    for title, t in tiles:
        d.text((20, y), title, font=title_font, fill=(40, 40, 40))
        out.paste(t, (20, y + 26))
        y += t.height + 40
    return out


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    sheet([
        ("Esquerda — layer Base, BT perfil 1 ligado", left()),
        ("Esquerda — layer Lower (ícone animado)", left("LOWER", "icon_lower_1", wpm=67)),
        ("Esquerda — layer Mouse (ativa ao usar o trackpad)", left("MOUSE", "icon_mouse_1", wpm=0)),
        ("Esquerda — por USB, a carregar", left("RAISE", "icon_raise_0", "sym_usb", "", "sym_ok", bl="chg", br=5, wpm=88)),
    ]).save(os.path.join(OUT, "oled_esquerda.png"))
    sheet([
        ("Direita — em repouso", right()),
        ("Direita — a escrever", right("pixo_type_0", mode="TYPE")),
        ("Direita — a mexer no trackpad", right("pixo_pad_1", dot=(24, 9), mode="MOVE", mimg="sym_tap")),
        ("Direita — scroll com 2 dedos", right("pixo_scroll_1", dot=(16, 12), mode="SCRL", mimg="sym_scroll")),
        ("Direita — a dormir (1 min sem uso)", right("pixo_sleep_0", mode="ZZZ", mimg="sym_zz")),
        ("Direita — sem ligação à esquerda", right("pixo_sad_0", link=False, mode="IDLE")),
    ]).save(os.path.join(OUT, "oled_direita.png"))
    print("ok")
