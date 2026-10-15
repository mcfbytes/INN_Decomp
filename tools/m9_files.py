#!/usr/bin/env python3
"""Compare files byte for byte between two drive trees.

usage: m9_files.py <drive_a> <drive_b> <relpath>...

Prints each path whose bytes differ or that is missing on one side, then a
one-line summary. Exit status is 1 when any path differs.
"""

import argparse
import sys
from pathlib import Path


def main(argv):
    parser = argparse.ArgumentParser(description='Compare files between two drive trees.')
    parser.add_argument('drive_a')
    parser.add_argument('drive_b')
    parser.add_argument('relpath', nargs='+')
    args = parser.parse_args(argv)
    differ = 0
    for rel in args.relpath:
        left = Path(args.drive_a) / rel
        right = Path(args.drive_b) / rel
        if not left.is_file() or not right.is_file():
            print('MISSING %s: a=%s b=%s' % (rel, left.is_file(), right.is_file()))
            differ += 1
            continue
        a = left.read_bytes()
        b = right.read_bytes()
        if a != b:
            print('DIFF %s: %d vs %d bytes' % (rel, len(a), len(b)))
            differ += 1
    print('files: %d compared, %d differ' % (len(args.relpath), differ))
    return 1 if differ else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
