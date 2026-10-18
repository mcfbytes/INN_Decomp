#!/usr/bin/env python3
"""Writes the page's 16 px icon, hand-pixelled because a painting that small turns to mud, and the .ico around it.

  tools/make_favicon.py [--out runtime/web/icons] [--paintings <dir of logo-32.png, logo-48.png>]

icon.svg is the 16 x 16 grid below, drawn with crisp edges so it scales up cleanly. favicon.ico holds the grid
rendered at 16 px and, when --paintings is given, the painted 32 and 48 px pictures from tools/make_logo.mjs
(--subject valley --time day --sizes 32,48), each stored as a PNG. Standard library only. The picture is the
valley by day: a cloud, the smoking volcano with its lava, the church spire, a cottage, autumn trees, the road.
"""
import argparse
import os
import struct
import zlib

GRID = """\
SSSSSSSSSSSSWWSS
SSSSSSSSSSSWWSSS
SWWWSSSSSSSSWSSS
WWWWWRSSSSSWSSSS
sssssRsssssLLsss
ssssRRRsssVLLVss
ssssRRRssVVVLVVs
ssssTTTsVVVVVLvv
RRRsTKTVVVVVVLvv
CCCsTTTVVVVVVVvv
CKCMTTTOOVVVVOOv
gggggTYYggggggOg
gOOggYYYYggggggg
OOOggYYYYYggggOO
gOgggYYYYYYgggOg
ggggYYYYYYYYgggg"""
PALETTE = dict(S='#6aaee8', s='#a6d2f0', W='#f8f4ec', R='#b8402c', T='#f0dcb0', K='#3a2a2a', V='#8c7480', v='#6c5464',
               L='#ff8c28', C='#e6d4aa', M='#6c8a3c', O='#e0742c', g='#7aa83a', Y='#e6cc94')


def rows():
    r = GRID.splitlines()
    assert len(r) == 16 and all(len(line) == 16 for line in r), [len(line) for line in r]
    return r


def svg():
    rects = [f"<rect x='{x}' y='{y}' width='1' height='1' fill='{PALETTE[c]}'/>"
             for y, line in enumerate(rows()) for x, c in enumerate(line) if c != 'S']
    return ("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16' shape-rendering='crispEdges'>"
            f"<rect width='16' height='16' fill='{PALETTE['S']}'/>" + ''.join(rects) + "</svg>\n")


def png16():
    raw = b''.join(b'\0' + b''.join(bytes.fromhex(PALETTE[c][1:]) for c in line) for line in rows())

    def chunk(kind, data):
        body = kind + data
        return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 16, 16, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def ico(images):
    """images: PNG byte strings, each square and under 256 px; the directory entry reads its size from the IHDR."""
    head = struct.pack('<HHH', 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries, data = b'', b''
    for png in images:
        w, h = struct.unpack('>II', png[16:24])
        entries += struct.pack('<BBBBHHII', w, h, 0, 0, 1, 32, len(png), offset + len(data))
        data += png
    return head + entries + data


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'runtime', 'web', 'icons'))
    ap.add_argument('--paintings', help='directory holding logo-32.png and logo-48.png')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, 'icon.svg'), 'w') as f:
        f.write(svg())
    images = [png16()]
    if a.paintings:
        for n in (32, 48):
            with open(os.path.join(a.paintings, f'logo-{n}.png'), 'rb') as f:
                images.append(f.read())
    with open(os.path.join(a.out, 'favicon.ico'), 'wb') as f:
        f.write(ico(images))


if __name__ == '__main__':
    main()
