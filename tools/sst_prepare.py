#!/usr/bin/env python3
"""Converts the SingleStepTests CPU suites into what runtime/tests/test_cpu_suite.c reads.

Fetch the suites first (test data never goes into the repo; work/ is git-ignored):

    cd work/cpu-tests
    git clone --depth 1 https://github.com/SingleStepTests/80386.git
    git clone --depth 1 https://github.com/SingleStepTests/80286.git
    git clone --depth 1 --filter=blob:none --sparse https://github.com/SingleStepTests/8088.git
    git -C 8088 sparse-checkout set v2_binary v2

Then: python3 -I tools/sst_prepare.py [work/cpu-tests]

Writes <dir>/prepared/<suite>/*.moo: the same MOO chunk format with the bus-cycle traces dropped
(they are most of the bytes and the harness does not check timing). For the 8088, whose MOO files
carry no undefined-flag mask, an RMSK chunk is built from v2/metadata.json, and opcodes the
metadata marks undefined are not converted. 286/386 files without their own mask get one from
the 80386 suite's 80386.csv (f_umask). Revoked test hashes go to <suite>/revoked.txt.
The test binary also takes INN_CPU_TESTS=<prepared dir>, INN_CPU_TESTS_VERBOSE=<failures shown
per file> and arguments selecting suites or files ("80386", "80386/0FBC", "6766F7.6").
"""
import csv
import gzip
import json
import pathlib
import struct
import sys

SUITES = {
    # name: (source dir, metadata json or None, revocation list)
    "8088": ("8088/v2_binary", "8088/v2/metadata.json", "8088/revocation_list.txt"),
    "80286": ("80286/v1_real_mode", None, "80286/revocation_list.txt"),
    "80386": ("80386/v1_ex_real_mode", None, "80386/revocation_list.txt"),
}
CSV_PATH = "80386/80386.csv"
RG32_EFLAGS_BIT = 17

REG_FLAGS_BIT = 13  # bit of "flags" in the 16-bit REGS/RMSK bitmask


def chunks(buf, off, end):
    while off + 8 <= end:
        kind = buf[off:off + 4]
        size = struct.unpack_from("<I", buf, off + 4)[0]
        yield kind, off + 8, size
        off += 8 + size


def chunk(kind, payload):
    return kind + struct.pack("<I", len(payload)) + payload


def strip_test(buf, off, size):
    out = [buf[off:off + 4]]  # test index
    for kind, o, n in chunks(buf, off + 4, off + size):
        if kind != b"CYCL":
            out.append(chunk(kind, buf[o:o + n]))
    return b"".join(out)


def metadata_entry(meta, stem):
    parts = stem.split(".")
    entry = meta["opcodes"].get(parts[0])
    if entry is None:
        return None
    if len(parts) > 1 and "reg" in entry:
        return entry["reg"].get(parts[1])
    return entry


def csv_masks(root):
    """(opcode, extension) -> undefined-flag mask from the 386 suite's opcode database."""
    path = root / CSV_PATH
    masks = {}
    if path.exists():
        with path.open(newline="") as f:
            for row in csv.DictReader(f):
                if row.get("f_umask"):
                    masks[(row["op"].upper(), row["ex"])] = int(row["f_umask"], 16)
    return masks


def csv_mask(masks, stem):
    parts = stem.split(".")
    op = parts[0]
    while len(op) > 2 and op[:2] in ("66", "67"):
        op = op[2:]
    ext = parts[1] if len(parts) > 1 else ""
    return masks.get((op, ext), masks.get((op, "")))


def convert(src, dst, flags_mask, wide):
    buf = gzip.open(src).read()
    has_mask = any(kind in (b"RMSK", b"RM32") for kind, _, _ in chunks(buf, 0, min(len(buf), 4096)))
    out = []
    for kind, o, n in chunks(buf, 0, len(buf)):
        if kind == b"TEST":
            out.append(chunk(kind, strip_test(buf, o, n)))
        else:
            out.append(chunk(kind, buf[o:o + n]))
            if kind == b"MOO " and flags_mask is not None and not has_mask:
                if wide:
                    out.append(chunk(b"RM32", struct.pack("<II", 1 << RG32_EFLAGS_BIT, flags_mask | 0xFFFF0000)))
                else:
                    out.append(chunk(b"RMSK", struct.pack("<HH", 1 << REG_FLAGS_BIT, flags_mask)))
    dst.write_bytes(b"".join(out))


def main():
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "work/cpu-tests")
    umasks = csv_masks(root)
    for name, (src_dir, meta_path, revoked) in SUITES.items():
        src = root / src_dir
        if not src.is_dir():
            print(f"{name}: {src} missing, skipped")
            continue
        meta = json.loads((root / meta_path).read_text()) if meta_path else None
        dst = root / "prepared" / name
        dst.mkdir(parents=True, exist_ok=True)
        rev = root / revoked
        hashes = [l.strip() for l in rev.read_text().splitlines() if len(l.strip()) == 40] if rev.exists() else []
        (dst / "revoked.txt").write_text("".join(h + "\n" for h in hashes))
        count = 0
        for path in sorted(src.glob("*.MOO.gz")):
            stem = path.name[:-len(".MOO.gz")]
            target = dst / (stem + ".moo")
            if target.exists() and target.stat().st_mtime >= path.stat().st_mtime:
                count += 1
                continue
            if meta is not None:
                entry = metadata_entry(meta, stem)
                if entry is None or entry.get("status") in ("undefined", "fpu", "prefix"):
                    continue
                flags_mask = entry.get("flags-mask", 0xFFFF)
            else:
                flags_mask = csv_mask(umasks, stem)
            convert(path, target, flags_mask, name == "80386")
            count += 1
        print(f"{name}: {count} files in {dst}")


if __name__ == "__main__":
    main()
