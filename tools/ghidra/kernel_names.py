#!/usr/bin/env python3
"""Print names.tsv lines from the Feb-1994 kernel table in interpreter.md."""

import re
import sys

ROW = re.compile(
    r"^\|\s*[0-9a-fA-F]+\s*\|\s*([^|]+?)\s*\|\s*([0-9a-fA-F]+):([0-9a-fA-F]+)\s*\|"
)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: kernel_names.py <interpreter.md>", file=sys.stderr)
        return 2
    lines = open(sys.argv[1], encoding="utf-8").read().splitlines()
    start = None
    for i, line in enumerate(lines):
        if line.startswith("### 3.3 Kernel table"):
            start = i + 1
            break
    if start is None:
        print("section 3.3 kernel table not found", file=sys.stderr)
        return 1
    count = 0
    for line in lines[start:]:
        if line.startswith("#"):
            break
        match = ROW.match(line)
        if not match:
            continue
        name = match.group(1).strip()
        seg = int(match.group(2), 16)
        off = int(match.group(3), 16)
        print(f"{seg:04x}:{off:04x}\tk{name}")
        count += 1
    if count == 0:
        print("no kernel rows parsed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
