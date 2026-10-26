#!/usr/bin/env python3
"""Lists the functions a run reached (INN_COVERAGE file) with their size and lift status.

usage: coverage_report.py <coverage file>... --ghidra <export dir> [--lifted <tree>]... [--module NAME]
       [--exe FILE] [--todo] [--leaf] [--unknown] [--tsv]
The export dir holds index.tsv (and overlays.tsv for Borland overlays); its name is the module name
unless --module gives it. Each lifted tree is scanned for LIFTED markers in the directory whose
module.txt names the module (*.c: lifted) and in staging/<that directory>/ (*.c.staged: staged).
Calls to an overlay's INT 3Fh thunk count for the overlay function it loads (thunks read from --exe,
default the file module.txt names): the overlay manager jumps there, which is no call.
An optional coverage_alias.tsv in the export dir lists overlays the manager moved after they were announced:
rows `<segment, hex, as the coverage file names it><TAB><overlay stub segment, hex><TAB>note`. Places the
run recorded inside that segment (module-relative, no `#`) count for the overlay's own offsets.
"""
import argparse
import collections
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import borland_overlay  # noqa: E402

MARKER = re.compile(r'^LIFTED\(\s*(0x[0-9A-Fa-f]+)\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*(\w+)', re.M)


def read_index(export):
    """Functions keyed by ('res', linear) or ('ovl', stub, offset in the overlay), and the stubs.
    Ghidra may split an overlay's flat segment, so overlay offsets are taken from the linear address."""
    overlays = []  # (flat linear, code bytes, stub)
    if (export / 'overlays.tsv').exists():
        for row in (export / 'overlays.tsv').read_text().splitlines()[1:]:
            flat, stub, code_bytes = row.split('\t')[:3]
            overlays.append((int(flat, 16) * 16, int(code_bytes), int(stub, 16)))
    funcs = {}
    for row in (export / 'index.tsv').read_text().splitlines()[1:]:
        addr, name, size, _callers, callees, _conv = row.split('\t')[:6]
        seg, off = (int(x, 16) for x in addr.split(':'))
        linear = seg * 16 + off
        key = ('res', linear)
        for flat, code_bytes, stub in overlays:
            if flat <= linear < flat + code_bytes:
                key = ('ovl', stub, linear - flat)
        funcs[key] = {'addr': addr, 'name': name, 'size': int(size), 'leaf': int(callees) == 0}
    return funcs, {stub for _flat, _bytes, stub in overlays}


def module_exe(trees, module):
    for tree in trees:
        for module_txt in tree.glob('*/module.txt'):
            lines = module_txt.read_text().split('\n')
            if lines[0].strip() == module and len(lines) > 1:
                return pathlib.Path(lines[1].strip())
    return None


def read_thunks(exe):
    """{(stub segment, thunk offset): overlay code offset} from the stubs' INT 3Fh thunks."""
    data = exe.read_bytes()
    header = int.from_bytes(data[8:10], 'little') * 16
    thunks = {}
    for stub, ovl in borland_overlay.overlays(data).items():
        for i in range(ovl.entries):
            at = header + stub * 16 + borland_overlay.STUB_THUNKS + borland_overlay.THUNK_BYTES * i
            thunks[(stub, borland_overlay.STUB_THUNKS + borland_overlay.THUNK_BYTES * i)] = \
                int.from_bytes(data[at + 2:at + 4], 'little')
    return thunks


def read_lifted(trees, module, stub_segs):
    status = {}
    for tree in trees:
        for module_txt in tree.glob('*/module.txt'):
            if module_txt.read_text().split('\n')[0].strip() != module:
                continue
            for kind, sources in (('staged', (tree / 'staging' / module_txt.parent.name).glob('*.c.staged')),
                                  ('lifted', module_txt.parent.glob('*.c'))):
                for source in sources:
                    for m in MARKER.finditer(source.read_text()):
                        seg, off = int(m[1], 16), int(m[2], 16)
                        key = ('ovl', seg, off) if seg in stub_segs else ('res', seg * 16 + off)
                        status[key] = kind  # lifted wins over staged: it is read last
    return status


def read_coverage(paths, module, thunks):
    calls = collections.Counter()
    for path in paths:
        for row in pathlib.Path(path).read_text().splitlines():
            if row.startswith('#') or not row.strip():
                continue
            place, count = row.split('\t')[:2]
            parts = place.split(':')
            name = parts[0]
            if name == module and len(parts) == 3:
                seg, off = int(parts[1], 16), int(parts[2], 16)
                if (seg, off) in thunks:
                    calls[('ovl', seg, thunks[(seg, off)])] += int(count)
                else:
                    calls[('res', seg * 16 + off)] += int(count)
            elif name.startswith(module + '#') and len(parts) == 2:
                calls[('ovl', int(name.split('#')[1], 16), int(parts[1], 16))] += int(count)
    return calls


def read_alias(export):
    """{module-relative segment: overlay stub segment} from coverage_alias.tsv, if the export has one."""
    alias = {}
    path = export / 'coverage_alias.tsv'
    if path.exists():
        for row in path.read_text().splitlines():
            if row.strip() and not row.startswith('#'):
                seg, stub = row.split('\t')[:2]
                alias[int(seg, 16)] = int(stub, 16)
    return alias


def apply_alias(calls, alias):
    merged = collections.Counter()
    for key, count in calls.items():
        if key[0] == 'res':
            for seg, stub in alias.items():
                if seg * 16 <= key[1] < seg * 16 + 0x10000:
                    key = ('ovl', stub, key[1] - seg * 16)
                    break
        merged[key] += count
    return merged


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('coverage', nargs='+')
    ap.add_argument('--ghidra', required=True, type=pathlib.Path)
    ap.add_argument('--lifted', action='append', default=[], type=pathlib.Path)
    ap.add_argument('--module')
    ap.add_argument('--exe', type=pathlib.Path)
    ap.add_argument('--unknown', action='store_true', help='also list targets that are no Ghidra function')
    ap.add_argument('--todo', action='store_true', help='only functions neither lifted nor staged')
    ap.add_argument('--leaf', action='store_true', help='only leaf functions (no callees)')
    ap.add_argument('--tsv', action='store_true')
    args = ap.parse_args()
    module = args.module or args.ghidra.resolve().name
    funcs, stub_segs = read_index(args.ghidra)
    status = read_lifted(args.lifted, module, stub_segs)
    exe = args.exe or (module_exe(args.lifted, module) if stub_segs else None)
    thunks = read_thunks(exe) if exe and exe.exists() else {}
    if stub_segs and not thunks:
        print('# no overlay thunks read: calls through them stay at the stub (--exe)', file=sys.stderr)
    calls = read_coverage(args.coverage, module, thunks)
    alias = read_alias(args.ghidra)
    if alias:
        calls = apply_alias(calls, alias)
    if not calls:
        sys.exit(f'no coverage rows for {module}')

    rows, unknown = [], []
    for key, count in calls.items():
        info = funcs.get(key)
        if not info:
            unknown.append(key)
            continue
        rows.append((key, info, count, status.get(key, '-')))
    rows.sort(key=lambda r: (r[0][0], r[0][1:]))
    totals = collections.Counter()
    for _key, info, _count, st in rows:
        totals[st] += 1
        totals[st + ' bytes'] += info['size']
    if args.todo:
        rows = [r for r in rows if r[3] == '-']
    if args.leaf:
        rows = [r for r in rows if r[1]['leaf']]

    sep = '\t' if args.tsv else '  '
    print(sep.join(['ghidra', 'overlay', 'name', 'size', 'calls', 'leaf', 'status']))
    for key, info, count, st in rows:
        cols = [info['addr'], f'{key[1]:04X}:{key[2]:04X}' if key[0] == 'ovl' else '-', info['name'], str(info['size']), str(count),
                'leaf' if info['leaf'] else '-', st]
        print(sep.join(cols))
    if args.unknown:
        for key in sorted(unknown):
            where = f'{key[1]:04X}:{key[2]:04X} (overlay)' if key[0] == 'ovl' else f'image +{key[1]:05X}'
            print(sep.join(['?', where, '?', '?', str(calls[key]), '?', status.get(key, '-')]))
    reached = len(calls) - len(unknown)
    print(f'# {module}: {reached} reached functions of {len(funcs)}; lifted {totals["lifted"]} '
          f'({totals["lifted bytes"]} bytes), staged {totals["staged"]} ({totals["staged bytes"]} bytes), '
          f'not lifted {totals["-"]} ({totals["- bytes"]} bytes); {len(unknown)} targets are no Ghidra function',
          file=sys.stderr)


if __name__ == '__main__':
    main()
