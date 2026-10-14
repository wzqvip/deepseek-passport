#!/usr/bin/env python3
"""Check the join code a device printed against the reference implementation.

The device prints its module matrix — `prov qr` does exactly that — and this compares it, cell for
cell, with the symbol the `qrcode` package produces for the same payload. That separates two questions
that have been tangled together since the code was first drawn: whether the **symbol** is correct, and
whether a **phone** can read it off the glass. This script answers the first one. The second is a
person's check, because it depends on the panel, the lighting and the camera.

Usage:
    python tools/check_device_qr.py captured.txt
    python tools/check_device_qr.py captured.txt --payload v3_wifi

`captured.txt` is the console output of `prov qr`, or just the matrix with the header line.
The payloads come from `make_qr_fixture.py`, which is the one place they are recorded — the device's
own payload contains the hotspot password, so it is matched rather than printed here.

The mask is the interesting part. The firmware picks one by scoring the specification's penalty rules,
and that choice has never had an independent oracle: the `segno` package was tried for it and disagreed
with the specification about a padding byte. So this does not check which mask *should* win. It checks
that the matrix the device drew is a valid symbol for the payload **under the mask the device says it
used**, which is a property the reference implementation can confirm.
"""

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from make_qr_fixture import CASES, add_byte_mode  # noqa: E402

import qrcode  # noqa: E402
from qrcode.constants import ERROR_CORRECT_L  # noqa: E402


def parse(output):
    """Return (version, modules, stated_mask, matrix) from `prov qr` output."""
    version = modules = stated_mask = None
    rows = []
    for line in output.splitlines():
        stripped = line.strip()
        header = re.search(r"version (\d+),\s*(\d+) modules,\s*mask (\d+)", stripped)
        if header:
            version = int(header.group(1))
            modules = int(header.group(2))
            stated_mask = int(header.group(3))
            continue
        if stripped and set(stripped) <= {"#", "."} and len(stripped) > 8:
            rows.append(stripped)
    if not rows:
        raise SystemExit("no matrix found in the input")
    return version, modules, stated_mask, rows


def strip_quiet_zone(rows):
    """The device prints a two-module border; the symbol itself is inside it."""
    if not rows:
        return rows
    inner = [row[2:-2] for row in rows[2:-2]]
    width = len(inner[0]) if inner else 0
    for row in inner:
        if len(row) != width:
            raise SystemExit("the matrix rows are not all the same width")
    return inner


def reference_matrix(payload, version, mask):
    qr = qrcode.QRCode(version=version, error_correction=ERROR_CORRECT_L, border=0)
    add_byte_mode(qr, payload)
    qr.makeImpl(False, mask)
    return ["".join("#" if cell else "." for cell in row) for row in qr.modules]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", help="file holding the output of `prov qr`")
    parser.add_argument("--payload", help="limit the comparison to one fixture payload by name")
    args = parser.parse_args()

    output = Path(args.capture).read_text(encoding="utf-8", errors="replace")
    version, modules, stated_mask, rows = parse(output)
    matrix = strip_quiet_zone(rows)

    print(f"device: version {version}, {modules} modules, states mask {stated_mask},"
          f" matrix {len(matrix)}x{len(matrix[0]) if matrix else 0}")
    if modules is not None and matrix and len(matrix) != modules:
        print(f"  note: the stated module count is {modules}, the matrix is {len(matrix)} wide")

    for name, payload, case_version in CASES:
        if args.payload and name != args.payload:
            continue
        if case_version != version:
            continue
        for mask in range(8):
            if reference_matrix(payload, case_version, mask) == matrix:
                if mask == stated_mask:
                    print(f"MATCH: the matrix is the reference symbol for fixture '{name}'"
                          f" under mask {mask}, which is the mask the device states")
                    return 0
                print(f"MISMATCH: the symbol is valid for fixture '{name}' under mask {mask},"
                      f" but the device states mask {stated_mask}")
                return 1

    print(f"NO MATCH: not the reference symbol for any fixture payload of version {version}"
          " under any of the eight masks")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
