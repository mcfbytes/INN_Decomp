#!/usr/bin/env python3
"""Compare two innkeeperd logs by client messages and host steps.

usage: m9_traffic.py <a.server.log> <b.server.log> [--ignore-kinds K1,K2]

Keeps, in order, the hex after `client message len=N` and the text after
`step=host `. ANSI colour codes are ignored. --ignore-kinds drops host steps
whose text starts with a listed kind. Prints
`traffic: N client messages, M host steps, identical` or the first difference
with five lines of context from each side. Exit status is 1 on a difference.
"""

import argparse
import re
import sys
from pathlib import Path

ANSI = re.compile(r'\x1b\[[0-9;]*[A-Za-z]')
CLIENT = re.compile(r'client message len=\d+\s*(.*)$')
HOST = re.compile(r'step=host (.*)$')


def events(path, ignore):
    text = ANSI.sub('', Path(path).read_text(errors='replace'))
    found = []
    clients = hosts = 0
    for line in text.splitlines():
        hits = []
        client = CLIENT.search(line)
        host = HOST.search(line)
        if client:
            hits.append((client.start(), 'C', client.group(1).strip()))
        if host:
            body = host.group(1).strip()
            if not any(body.startswith(kind) for kind in ignore):
                hits.append((host.start(), 'H', body))
        for _pos, kind, body in sorted(hits):
            found.append((kind, body))
            if kind == 'C':
                clients += 1
            else:
                hosts += 1
    return found, clients, hosts


def show(label, rows, index):
    print('--- %s' % label)
    start = max(0, index - 5)
    end = min(len(rows), index + 6)
    for i in range(start, end):
        mark = '>' if i == index else ' '
        kind, body = rows[i]
        print('%s %d %s %s' % (mark, i, kind, body))
    if index >= len(rows):
        print('> %d (end)' % index)


def main(argv):
    parser = argparse.ArgumentParser(description='Compare innkeeperd client messages and host steps.')
    parser.add_argument('a')
    parser.add_argument('b')
    parser.add_argument('--ignore-kinds', default='', help='comma-separated host-step prefixes to drop')
    args = parser.parse_args(argv)
    ignore = [kind for kind in args.ignore_kinds.split(',') if kind]
    left, n_left, m_left = events(args.a, ignore)
    right, n_right, m_right = events(args.b, ignore)
    limit = min(len(left), len(right))
    differ = next((i for i in range(limit) if left[i] != right[i]), None)
    if differ is None and len(left) == len(right):
        print('traffic: %d client messages, %d host steps, identical' % (n_left, m_left))
        return 0
    if differ is None:
        differ = limit
    print('traffic: differ at event %d (a: %d messages, %d steps; b: %d messages, %d steps)' % (
        differ, n_left, m_left, n_right, m_right))
    show('a', left, differ)
    show('b', right, differ)
    return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
