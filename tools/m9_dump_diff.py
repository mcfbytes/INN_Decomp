#!/usr/bin/env python3
"""Compare two INN_DUMP_AT memory dumps and their sidecar text files.

usage: m9_dump_diff.py <a.bin> <b.bin> [--ignore START-END ...]

The dump is the guest memory image (package 10a writes 0 through 10FFEFh) and
<name>.txt beside it. Differing bytes outside the ignored ranges are merged
when the next difference is fewer than 16 bytes after the previous one. Each
range is printed with the first 16 bytes of each side. The text files are
compared line by line. Exit status is 1 on a difference outside the ignored
ranges. START and END are hex, inclusive.
"""

import argparse
import sys
from pathlib import Path


def parse_range(text):
    start_s, end_s = text.split('-', 1)
    start, end = int(start_s, 16), int(end_s, 16)
    if end < start:
        raise ValueError('ignore range %s ends before it starts' % text)
    return start, end


def ignored(offset, ranges):
    return any(start <= offset <= end for start, end in ranges)


def differing_offsets(a, b, ranges):
    limit = min(len(a), len(b))
    found = [i for i in range(limit) if a[i] != b[i] and not ignored(i, ranges)]
    longer = a if len(a) >= len(b) else b
    found.extend(i for i in range(limit, len(longer)) if not ignored(i, ranges))
    return found, len(a) != len(b)


def merge_ranges(offsets):
    if not offsets:
        return []
    ranges = []
    start = prev = offsets[0]
    for offset in offsets[1:]:
        if offset - prev < 16:
            prev = offset
            continue
        ranges.append((start, prev))
        start = prev = offset
    ranges.append((start, prev))
    return ranges


def hex16(data, start):
    return ' '.join('%02X' % byte for byte in data[start:start + 16])


def diff_text(a_path, b_path):
    problems = 0
    if not a_path.is_file() or not b_path.is_file():
        print('txt missing: a=%s b=%s' % (a_path.is_file(), b_path.is_file()))
        return 1
    left = a_path.read_text(errors='replace').splitlines()
    right = b_path.read_text(errors='replace').splitlines()
    for index in range(max(len(left), len(right))):
        a_line = left[index] if index < len(left) else None
        b_line = right[index] if index < len(right) else None
        if a_line == b_line:
            continue
        problems += 1
        print('txt %d' % (index + 1))
        print('  a: %s' % ('' if a_line is None else a_line))
        print('  b: %s' % ('' if b_line is None else b_line))
    return problems


def main(argv):
    parser = argparse.ArgumentParser(description='Compare two INN_DUMP_AT memory dumps.')
    parser.add_argument('a')
    parser.add_argument('b')
    parser.add_argument('--ignore', action='append', default=[], metavar='START-END',
                        help='inclusive hex range to skip; may be repeated or comma-separated')
    args = parser.parse_args(argv)
    ranges = []
    try:
        for item in args.ignore:
            for part in item.split(','):
                if part:
                    ranges.append(parse_range(part))
    except ValueError as error:
        print('dump: %s' % error)
        return 1
    a_path, b_path = Path(args.a), Path(args.b)
    a, b = a_path.read_bytes(), b_path.read_bytes()
    offsets, _length_diff = differing_offsets(a, b, ranges)
    merged = merge_ranges(offsets)
    for start, end in merged:
        print('%08X-%08X (%d bytes)' % (start, end, end - start + 1))
        print('  a: %s' % hex16(a, start))
        print('  b: %s' % hex16(b, start))
    text_problems = diff_text(a_path.with_suffix('.txt'), b_path.with_suffix('.txt'))
    if not merged and text_problems == 0:
        hidden = any(
            (i < len(a) and i < len(b) and a[i] != b[i] and ignored(i, ranges))
            or (i >= min(len(a), len(b)) and ignored(i, ranges))
            for i in range(max(len(a), len(b)))
        )
        if hidden:
            print('dump: identical outside ignored ranges')
        else:
            print('dump: identical')
        return 0
    print('dump: %d ranges, %d txt lines differ' % (len(merged), text_problems))
    return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
