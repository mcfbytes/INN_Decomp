#!/usr/bin/env python3
"""Diff two scenario-suite directories by verification counts.

usage: m9_suite_diff.py <base_dir> <new_dir> [--allow name,...]

A key is "<module> <function>" for a live summary, or the same with
" (synthetic)" or " (device replay)". Several lifted entries share one
function name, so every summary of a key is added: a uniq -c prefix weights
that line, and a line with no prefix counts as one. FAIL when a status leaves
ok, a key's mismatch count rises, or a key that checked calls in the base has
none in the new run (coverage lost). --allow skips the coverage check for
those keys. <base_dir>/FLAKY, one key per line, turns that key's failures into
warnings. Checked counts that fall but stay above zero, and keys only in the
new run, are warnings. Exit status is 1 when any FAIL remains.
"""

import argparse
import re
import sys
from pathlib import Path

VERIFY_RE = re.compile(
    r'^(?:\s*(\d+)\s+)?verify: (\S+) (\S+): (\d+) calls checked, (\d+) mismatches'
    r'(?:, DX:AX differs in \d+)?(?: \((synthetic|device replay)\))?\s*$'
)


def key_of(module, fn, variant):
    if variant:
        return '%s %s (%s)' % (module, fn, variant)
    return '%s %s' % (module, fn)


def parse_verify(path):
    found = {}
    if not path.is_file():
        return found
    for line in path.read_text(errors='replace').splitlines():
        match = VERIFY_RE.match(line)
        if not match:
            continue
        copies = int(match.group(1)) if match.group(1) else 1
        key = key_of(match.group(2), match.group(3), match.group(6))
        checked = copies * int(match.group(4))
        mismatches = copies * int(match.group(5))
        prev = found.get(key)
        if prev:
            found[key] = (prev[0] + checked, prev[1] + mismatches)
        else:
            found[key] = (checked, mismatches)
    return found


def read_status(path):
    if not path.is_file():
        return None
    lines = path.read_text(errors='replace').splitlines()
    return lines[0] if lines else ''


def scenario_names(directory):
    return {path.name[:-len('.status')] for path in Path(directory).glob('*.status')}


def read_flaky(base):
    path = Path(base) / 'FLAKY'
    keys = set()
    if not path.is_file():
        return keys
    for line in path.read_text(errors='replace').splitlines():
        line = line.split('#', 1)[0].strip()
        if line:
            keys.add(line)
    return keys


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('base_dir')
    parser.add_argument('new_dir')
    parser.add_argument('--allow', default='', help='comma-separated keys exempt from coverage loss')
    args = parser.parse_args(argv)
    allow = {item.strip() for item in args.allow.split(',') if item.strip()}
    flaky = read_flaky(args.base_dir)
    base = Path(args.base_dir)
    new = Path(args.new_dir)
    common = sorted(scenario_names(base) & scenario_names(new))
    fails = 0
    warns = 0

    def add(kind, scenario, message, key=None):
        nonlocal fails, warns
        if key and key in flaky and kind == 'FAIL':
            kind = 'WARN'
            message += ' (flaky)'
        print('%s %s: %s' % (kind, scenario, message))
        if kind == 'FAIL':
            fails += 1
        else:
            warns += 1

    for name in common:
        base_status = read_status(base / (name + '.status'))
        new_status = read_status(new / (name + '.status'))
        if base_status == 'ok' and new_status != 'ok':
            add('FAIL', name, 'status ok -> %s' % new_status)
        base_keys = parse_verify(base / (name + '.verify'))
        new_keys = parse_verify(new / (name + '.verify'))
        for key, (checked, mismatches) in base_keys.items():
            if key not in new_keys:
                if checked > 0 and key not in allow:
                    add('FAIL', name, '%s coverage lost (%d -> missing)' % (key, checked), key)
                continue
            new_checked, new_mismatches = new_keys[key]
            if new_mismatches > mismatches:
                add('FAIL', name, '%s mismatches %d -> %d' % (key, mismatches, new_mismatches), key)
            if checked > 0 and new_checked == 0:
                if key not in allow:
                    add('FAIL', name, '%s coverage lost (%d -> 0)' % (key, checked), key)
            elif 0 < new_checked < checked:
                add('WARN', name, '%s checked %d -> %d' % (key, checked, new_checked))
        for key, (checked, mismatches) in new_keys.items():
            if key not in base_keys:
                add('WARN', name, '%s new (%d calls, %d mismatches)' % (key, checked, mismatches))
    print('diff: %d scenarios, %d FAIL, %d WARN' % (len(common), fails, warns))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
