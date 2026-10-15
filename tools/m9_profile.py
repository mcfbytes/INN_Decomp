#!/usr/bin/env python3
"""Summarise an INN_INTERP_PROFILE file.

usage: m9_profile.py <profile.txt> [--top N]

Lines are `module <name> <load segment hex>`, `direct <n> fallbacks <n>
callbacks <n>` (further fields are kept and printed), and `<paragraph hex>
<count>`. Paragraphs at F000 and above are BIOS/ROM. Any other paragraph
belongs to the loaded module with the highest load segment not above it.
The top paragraphs are printed as `<module> image para <paragraph - load segment>`.
"""

import argparse
import re
import sys
from pathlib import Path

MODULE_RE = re.compile(r'^module\s+(\S+)\s+([0-9A-Fa-f]+)\s*$')
PARA_RE = re.compile(r'^([0-9A-Fa-f]+)\s+(\d+)\s*$')
DIRECT_RE = re.compile(r'^direct\s+\d+\s+fallbacks\s+\d+\s+callbacks\s+\d+\b')
ROM_SEG = 0xF000


def load(path):
    modules = []
    counts = []
    direct = None
    for raw in Path(path).read_text(errors='replace').splitlines():
        line = raw.split('#', 1)[0].strip()
        if not line:
            continue
        module = MODULE_RE.match(line)
        if module:
            modules.append((module.group(1), int(module.group(2), 16), len(modules)))
            continue
        if line.startswith('direct ') and DIRECT_RE.match(line):
            direct = line
            continue
        para = PARA_RE.match(line)
        if para:
            counts.append((int(para.group(1), 16), int(para.group(2))))
    return modules, counts, direct


def owner(para, modules):
    """Return (module name, load segment) for a paragraph."""
    if para >= ROM_SEG:
        return 'BIOS/ROM', ROM_SEG
    best = None
    for name, seg, index in modules:
        if seg <= para and (best is None or seg > best[1] or (seg == best[1] and index > best[2])):
            best = (name, seg, index)
    if best is None:
        return 'unassigned', 0
    return best[0], best[1]


def main(argv):
    parser = argparse.ArgumentParser(description='Summarise an INN_INTERP_PROFILE file.')
    parser.add_argument('profile')
    parser.add_argument('--top', type=int, default=20, help='how many paragraphs to list (default 20)')
    args = parser.parse_args(argv)
    modules, counts, direct = load(args.profile)
    print(direct if direct is not None else 'direct (absent)')
    totals = {}
    order = []
    for name, seg, _index in modules:
        if name not in totals:
            order.append(name)
            totals[name] = 0
    grand = 0
    ranked = []
    for para, count in counts:
        name, seg = owner(para, modules)
        grand += count
        if name not in totals:
            order.append(name)
            totals[name] = 0
        totals[name] += count
        ranked.append((count, para, name, para - seg))
    print('total %d' % grand)
    if 'unassigned' in totals and 'unassigned' not in order:
        order.insert(0, 'unassigned')
    elif 'unassigned' in totals:
        order.remove('unassigned')
        order.insert(0, 'unassigned')
    if 'BIOS/ROM' not in totals:
        totals['BIOS/ROM'] = 0
    if 'BIOS/ROM' in order:
        order.remove('BIOS/ROM')
    order.append('BIOS/ROM')
    for name in order:
        count = totals[name]
        pct = (100.0 * count / grand) if grand else 0.0
        print('%s %d %.1f%%' % (name, count, pct))
    print('top:')
    ranked.sort(key=lambda item: (-item[0], item[1]))
    for count, _para, name, image in ranked[:max(args.top, 0)]:
        print('%s image para %04X %d' % (name, image, count))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
