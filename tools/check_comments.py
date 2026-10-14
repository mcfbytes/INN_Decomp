#!/usr/bin/env python3
"""Flag comment blocks longer than two lines in .c, .h and .py files.

A run of lines that are only // comments (C) or # comments (Python) is one block.
A /* ... */ comment is one block of its physical line count. A leading #! line
is ignored and docstrings are exempt. Markers inside string or character literals
are not comments. Scans runtime/, recomp/ and tools/, or the paths given as
arguments, and skips every third_party directory.
"""

import io
import os
import sys
import tokenize
from pathlib import Path

EXTS = {'.c', '.h', '.py'}
WS = ' \t\v\f'
SKIP_TOKENS = {
    tokenize.COMMENT, tokenize.NL, tokenize.NEWLINE, tokenize.INDENT,
    tokenize.DEDENT, tokenize.ENDMARKER, tokenize.ENCODING,
}


def overlong(nums):
    found, start, prev = [], None, None
    for num in nums:
        if prev is None:
            start = num
        elif num != prev + 1:
            if prev - start + 1 > 2:
                found.append((start, prev - start + 1))
            start = num
        prev = num
    if prev is not None and prev - start + 1 > 2:
        found.append((start, prev - start + 1))
    return found


def check_c(text):
    found, mode, block_at, slash_lines = [], 'code', 0, []
    lines = text.splitlines()
    for no, line in enumerate(lines, 1):
        pure, slash, i, n = mode == 'code', False, 0, len(line)
        while i < n:
            c = line[i]
            if mode == 'block':
                pure = False
                if c == '*' and i + 1 < n and line[i + 1] == '/':
                    if no - block_at + 1 > 2:
                        found.append((block_at, no - block_at + 1))
                    mode, block_at, i = 'code', 0, i + 2
                else:
                    i += 1
                continue
            if mode != 'code':
                pure = False
                if c == '\\':
                    if i + 1 >= n:
                        break
                    i += 2
                    continue
                if c == mode:
                    mode = 'code'
                i += 1
                continue
            if c in WS:
                i += 1
                continue
            if c == '/' and i + 1 < n and line[i + 1] == '/':
                slash = True
                break
            if c == '/' and i + 1 < n and line[i + 1] == '*':
                pure, mode, block_at, i = False, 'block', no, i + 2
                continue
            if c in '"\'':
                pure, mode, i = False, c, i + 1
                continue
            pure, i = False, i + 1
        if mode not in ('code', 'block') and not line.endswith('\\'):
            mode = 'code'
        if slash and pure:
            slash_lines.append(no)
    if mode == 'block' and len(lines) - block_at + 1 > 2:
        found.append((block_at, len(lines) - block_at + 1))
    found.extend(overlong(slash_lines))
    found.sort()
    return found


def check_py(text):
    comments, code = set(), set()
    try:
        for kind, _, start, end, _ in tokenize.generate_tokens(io.StringIO(text).readline):
            if kind == tokenize.COMMENT:
                comments.add(start[0])
            elif kind not in SKIP_TOKENS:
                last = end[0] - 1 if end[1] == 0 and end[0] > start[0] else end[0]
                code.update(range(start[0], last + 1))
    except (tokenize.TokenError, IndentationError, SyntaxError):
        pass
    if text.startswith('#!'):
        comments.discard(1)
    return overlong(sorted(comments - code))


def source_files(roots):
    found = []
    for raw in roots:
        root = Path(raw)
        if not root.exists() or 'third_party' in root.parts:
            continue
        if root.is_file():
            if root.suffix in EXTS:
                found.append(root)
            continue
        if not root.is_dir():
            continue
        for dirpath, dirnames, names in os.walk(root):
            dirnames[:] = [name for name in dirnames if name != 'third_party']
            for name in names:
                path = Path(dirpath) / name
                if path.suffix in EXTS:
                    found.append(path)
    return sorted(found)


def main(argv):
    failed = False
    roots = argv if argv else ['runtime', 'recomp', 'tools']
    for path in source_files(roots):
        text = path.read_text(encoding='utf-8', errors='replace')
        blocks = check_py(text) if path.suffix == '.py' else check_c(text)
        for line, length in blocks:
            print(f'{path.as_posix()}:{line}: {length}-line comment block')
            failed = True
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
