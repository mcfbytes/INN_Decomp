#!/usr/bin/env python3
"""Compare two screenshots written by tools/ppm2png.py, or two binary PPMs.

usage: m9_screens.py <a> <b> [--tolerant PCT]

Exact mode requires identical pixels. --tolerant PCT allows at most PCT percent
of pixels to differ, and still requires the same dimensions. Prints the
differing pixel count and the bounding box. Exit status is 1 on failure.
"""

import argparse
import struct
import sys
import zlib
from pathlib import Path


def read_ppm(data):
    fields, pos = [], 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b'#':
            pos = data.index(b'\n', pos)
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[pos:end])
        pos = end
    if fields[0] != b'P6':
        raise ValueError('not a binary PPM')
    width, height = int(fields[1]), int(fields[2])
    rgb = data[pos + 1:pos + 1 + width * height * 3]
    if len(rgb) != width * height * 3:
        raise ValueError('truncated PPM')
    return width, height, rgb


def read_png(data):
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG')
    pos = 8
    width = height = None
    idat = []
    while pos + 8 <= len(data):
        length = struct.unpack('>I', data[pos:pos + 4])[0]
        tag = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        crc = struct.unpack('>I', data[pos + 8 + length:pos + 12 + length])[0]
        if (zlib.crc32(tag + body) & 0xFFFFFFFF) != crc:
            raise ValueError('bad PNG checksum')
        pos += 12 + length
        if tag == b'IHDR':
            width, height, depth, color, comp, filt, inter = struct.unpack('>IIBBBBB', body)
            if (depth, color, comp, filt, inter) != (8, 2, 0, 0, 0):
                raise ValueError('unsupported PNG (need 8-bit RGB)')
        elif tag == b'IDAT':
            idat.append(body)
        elif tag == b'IEND':
            break
    if width is None:
        raise ValueError('PNG has no header')
    raw = zlib.decompress(b''.join(idat))
    stride = width * 3
    rows = []
    cursor = 0
    for _y in range(height):
        if cursor >= len(raw) or raw[cursor] != 0:
            raise ValueError('unsupported PNG row filter')
        cursor += 1
        rows.append(raw[cursor:cursor + stride])
        cursor += stride
    rgb = b''.join(rows)
    if len(rgb) != width * height * 3:
        raise ValueError('truncated PNG')
    return width, height, rgb


def read_image(path):
    data = Path(path).read_bytes()
    if data.startswith(b'\x89PNG'):
        return read_png(data)
    if data.startswith(b'P6'):
        return read_ppm(data)
    raise ValueError('not a PNG or binary PPM')


def bounding_box(width, height, a, b):
    x0 = y0 = x1 = y1 = None
    count = 0
    for y in range(height):
        row = y * width * 3
        for x in range(width):
            i = row + x * 3
            if a[i:i + 3] != b[i:i + 3]:
                count += 1
                if x0 is None:
                    x0 = x1 = x
                    y0 = y1 = y
                else:
                    x0 = min(x0, x)
                    y0 = min(y0, y)
                    x1 = max(x1, x)
                    y1 = max(y1, y)
    return count, (None if x0 is None else (x0, y0, x1, y1))


def main(argv):
    parser = argparse.ArgumentParser(description='Compare two PPM or PNG screenshots.')
    parser.add_argument('a')
    parser.add_argument('b')
    parser.add_argument('--tolerant', type=float, default=None, metavar='PCT',
                        help='allow up to PCT percent of pixels to differ')
    args = parser.parse_args(argv)
    try:
        aw, ah, a = read_image(args.a)
        bw, bh, b = read_image(args.b)
    except (OSError, ValueError, zlib.error) as error:
        print('screens: %s' % error)
        return 1
    if (aw, ah) != (bw, bh):
        print('screens: size %dx%d vs %dx%d' % (aw, ah, bw, bh))
        return 1
    count, box = bounding_box(aw, ah, a, b)
    total = aw * ah
    pct = (100.0 * count / total) if total else 0.0
    print('differing pixels: %d / %d (%.4f%%)' % (count, total, pct))
    if box is None:
        print('bounding box: empty')
    else:
        print('bounding box: %d,%d %d,%d' % box)
    if args.tolerant is None:
        ok = count == 0
        limit = 'exact'
    else:
        ok = pct <= args.tolerant
        limit = 'tolerant %.4g%%' % args.tolerant
    if not ok:
        print('screens: differ (%s)' % limit)
        return 1
    print('screens: match (%s)' % limit)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
