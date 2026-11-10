# WRSR Mail - a postal service for Workers & Resources: Soviet Republic
# Copyright (C) 2026 TheRealMeowMeow00
# SPDX-License-Identifier: GPL-3.0-only
#
# Draws the resource icons: 48x48 RGBA, which is what the game loads from
# resources/<name>.png. Drawn at 8x and reduced, so edges are antialiased.
#
#   python tools/make_icons.py

import os
from PIL import Image, ImageDraw, ImageFilter

S = 8
SIZE = 48 * S
OUTLINE = (38, 32, 26, 255)
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "item", "vfs", "media_soviet", "resources")


def p(*xy):
    """Scale 48-grid coordinates to the working canvas."""
    return [v * S for v in xy]


def envelope(size_wh, body, shade, flap, flap_dark, sealed):
    """One envelope on its own layer, with a soft lower shade and a dark outline."""
    w, h = size_wh
    pad = 4
    im = Image.new("RGBA", ((w + pad * 2) * S, (h + pad * 2) * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    x0, y0, x1, y1 = pad, pad, pad + w, pad + h
    d.rounded_rectangle(p(x0, y0, x1, y1), radius=S, fill=body, outline=OUTLINE, width=S)
    # The lower folds meet in the middle.
    mid = (x0 + x1) / 2
    d.polygon(p(x0 + 1, y1 - 1, mid, y0 + h * 0.46, x1 - 1, y1 - 1), fill=shade)
    if sealed:
        d.polygon(p(x0 + 0.6, y0 + 0.6, x1 - 0.6, y0 + 0.6, mid, y0 + h * 0.62), fill=flap)
        d.line(p(x0 + 0.6, y0 + 0.6, mid, y0 + h * 0.62, x1 - 0.6, y0 + 0.6), fill=flap_dark, width=S)
    else:
        d.line(p(x0 + 1, y1 - 1, mid, y0 + h * 0.46, x1 - 1, y1 - 1), fill=flap_dark, width=S)
    d.rounded_rectangle(p(x0, y0, x1, y1), radius=S, outline=OUTLINE, width=S)
    return im


def paste_rot(base, layer, angle, cx, cy):
    r = layer.rotate(angle, resample=Image.BICUBIC, expand=True)
    base.alpha_composite(r, (int(cx * S - r.width / 2), int(cy * S - r.height / 2)))


def shadow(base):
    a = base.getchannel("A").filter(ImageFilter.GaussianBlur(1.2 * S))
    sh = Image.new("RGBA", base.size, (0, 0, 0, 0))
    sh.putalpha(a.point(lambda v: int(v * 0.35)))
    out = Image.new("RGBA", base.size, (0, 0, 0, 0))
    out.alpha_composite(sh, (0, int(0.8 * S)))
    out.alpha_composite(base)
    return out


def finish(im, name):
    os.makedirs(OUT, exist_ok=True)
    small = im.resize((48, 48), Image.LANCZOS)
    small.save(os.path.join(OUT, name + ".png"))
    return small


def mail_raw():
    """Unprocessed: a loose, worn bundle tied with string."""
    im = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    worn = [
        ((196, 176, 140, 255), (168, 148, 112, 255)),
        ((214, 198, 164, 255), (186, 168, 132, 255)),
        ((204, 186, 150, 255), (176, 156, 120, 255)),
    ]
    for (body, sh), angle, cx, cy in [
        (worn[0], -13, 24, 31),
        (worn[1], 9, 24, 25.5),
        (worn[2], -5, 24, 19.5),
    ]:
        paste_rot(im, envelope((34, 20), body, sh, body, (120, 100, 72, 255), False), angle, cx, cy)
    # The string round the bundle.
    d = ImageDraw.Draw(im)
    d.line(p(24.5, 6.5, 23.5, 41.5), fill=OUTLINE, width=int(3.4 * S))
    d.line(p(24.5, 6.5, 23.5, 41.5), fill=(150, 62, 40, 255), width=int(1.9 * S))
    d.ellipse(p(21.2, 14.8, 27.2, 20.8), fill=(150, 62, 40, 255), outline=OUTLINE, width=int(0.8 * S))
    return shadow(im)


def mail_done():
    """Processed: one clean sealed envelope with a stamp and a sorting stripe."""
    im = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    env = envelope((38, 26), (240, 240, 232, 255), (222, 224, 214, 255),
                   (250, 250, 244, 255), (150, 152, 144, 255), True)
    d = ImageDraw.Draw(env)
    # The stamp, top right, and an airmail-style edge band along the bottom.
    d.rectangle(p(32, 7, 39.5, 15), fill=(186, 38, 34, 255), outline=(255, 255, 255, 255), width=int(0.8 * S))
    for i, x in enumerate(range(6, 38, 5)):
        c = (186, 38, 34, 255) if i % 2 == 0 else (34, 76, 150, 255)
        d.polygon(p(x, 28.4, x + 2.8, 28.4, x + 1.4, 26.2), fill=c)
    paste_rot(im, env, 4, 24, 24)
    return shadow(im)


def bottomtab_mail():
    """The build menu tab: 96x96 like the game's editor/bottomtab_*.png, a paper card with
    the sealed envelope on it."""
    im = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle(p(4, 4, 44, 44), radius=int(2.5 * S), fill=(246, 232, 198, 255),
                        outline=(176, 150, 108, 255), width=int(0.9 * S))
    d.rounded_rectangle(p(6, 6, 42, 42), radius=int(1.5 * S), outline=(226, 206, 166, 255), width=int(0.6 * S))
    env = mail_done()
    k = 0.74
    env = env.resize((int(SIZE * k), int(SIZE * k)), Image.LANCZOS)
    im.alpha_composite(env, ((SIZE - env.width) // 2, (SIZE - env.height) // 2))
    out = os.path.join(HERE, "..", "item", "vfs", "media_soviet", "editor")
    os.makedirs(out, exist_ok=True)
    small = im.resize((96, 96), Image.LANCZOS)
    small.save(os.path.join(out, "bottomtab_mail.png"))
    return small


if __name__ == "__main__":
    a = finish(mail_raw(), "mail_raw")
    b = finish(mail_done(), "mail_done")
    t = bottomtab_mail()

    # A contact sheet for review: true size and x6, on light and dark.
    sheet = Image.new("RGBA", (660, 300), (120, 120, 120, 255))
    d = ImageDraw.Draw(sheet)
    d.rectangle((330, 0, 660, 300), fill=(44, 44, 48, 255))
    for ox in (0, 330):
        sheet.alpha_composite(a, (ox + 12, 12))
        sheet.alpha_composite(b, (ox + 12, 72))
        sheet.alpha_composite(a.resize((144, 144), Image.NEAREST), (ox + 72, 12))
        sheet.alpha_composite(b.resize((144, 144), Image.NEAREST), (ox + 72, 156))
    sheet.alpha_composite(t, (230, 12))
    sheet.alpha_composite(t, (560, 12))
    sheet.save(os.path.join(HERE, "..", "out", "icons_preview.png"))
    print("ok", a.size, a.mode, b.size, b.mode)
