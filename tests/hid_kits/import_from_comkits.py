#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Pull community-contributed captures from comkits/ into tests/hid_kits/.

Two different physical instruments can share a VID:PID (e.g. a Santroller
flashed as a Guitar Hero 5 clone vs. as a Pro Drum), so when there are
multiple kits at the same VID:PID we disambiguate the filename with a
short SHA-1 of the device name:

    kit_<vid>_<pid>.raw.jsonl          # the canonical / first capture
    kit_<vid>_<pid>_<hash>.raw.jsonl   # alternates from other devices

The test harness (tests/hidtest/main.cpp) recognises both forms — the
filename parser uses the first two underscore-separated chunks after the
`kit_` prefix and ignores anything past them. The meta header carries the
ground-truth VID/PID and device_name so even hash-suffixed fixtures route
through DeriveKitToml correctly.

This script is idempotent: running it multiple times produces the same
filenames and overwrites the previous content with the latest source.

Usage:
    python3 tests/hid_kits/import_from_comkits.py
"""

import hashlib
import json
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
SRC_ROOTS = [REPO / "comkits" / d for d in ("kits", "kits2", "kits3", "drum")]
DST = REPO / "tests" / "hid_kits"

# Some VID:PIDs have BOTH a HID and an XInput capture. The XInput one
# exercises FillXInputReport + the bitmap-d-pad packer path, which is
# otherwise uncovered. Prefer the XInput capture as the canonical
# fixture for those VID:PIDs so the test corpus keeps that coverage.
PREFER_XINPUT = {"1209_2882"}


def parse_toml(path: pathlib.Path) -> dict:
    out = {}
    for line in path.read_text().splitlines():
        m = re.match(r'(\w+)\s*=\s*"([^"]*)"', line)
        if m:
            out[m.group(1)] = m.group(2)
            continue
        m = re.match(r'(\w+)\s*=\s*(\S+)', line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


def short_hash(name: str) -> str:
    return hashlib.sha1(name.encode()).hexdigest()[:8]


def hex4(value: str) -> str:
    """Normalize `0x100` and `0x0100` to a 4-digit lowercase form."""
    return f"{int(value, 16):04x}"


def import_one(toml_path: pathlib.Path, has_canonical: dict[str, bool]) -> None:
    raw = toml_path.with_suffix(".raw.jsonl")
    if not raw.exists():
        return
    fields = parse_toml(toml_path)
    name = fields.get("name", "(unnamed)")
    vid_raw = fields.get("vendor_id", "0x0").strip('"')
    pid_raw = fields.get("product_id", "0x0").strip('"')
    vid = hex4(vid_raw)
    pid = hex4(pid_raw)
    key = f"{vid}_{pid}"
    # First-seen capture per VID:PID keeps the un-hashed filename so the
    # most common kit can be referenced without remembering its hash. For
    # VID:PIDs in PREFER_XINPUT, defer claiming canonical until we see an
    # xinput-source toml.
    source = fields.get("source", "hid")
    canonical_taken = has_canonical.get(key, False)
    if key in PREFER_XINPUT and not canonical_taken and source != "xinput":
        # First-seen HID capture goes to a hashed slot; the xinput one
        # (which arrives later under kits2/) gets the canonical name.
        dst = DST / f"kit_{vid}_{pid}_{short_hash(name)}.raw.jsonl"
    elif not canonical_taken:
        has_canonical[key] = True
        dst = DST / f"kit_{vid}_{pid}.raw.jsonl"
    else:
        dst = DST / f"kit_{vid}_{pid}_{short_hash(name)}.raw.jsonl"

    device_class = fields.get("device_class", "guitar")
    if device_class == "drum":
        device_type = "drum_pro" if fields.get("drum_ps4_layout") == "true" else "drum"
    else:
        device_type = "guitar_solo" if fields.get("solo_fret_byte") else "guitar"

    meta = {
        "type": "meta",
        "version": 2,
        "vendor_id": f"0x{vid}",
        "product_id": f"0x{pid}",
        "device_name": name,
        "device_type": device_type,
        "source": fields.get("source", "hid"),
        "report_length": int(fields.get("report_length", 0)),
    }
    raw_lines = raw.read_text().splitlines()
    out_lines = [json.dumps(meta, separators=(",", ":"))] + raw_lines
    dst.write_text("\n".join(out_lines) + "\n")
    print(f"{dst.name:<48} {name}")


def main() -> int:
    has_canonical: dict[str, bool] = {}
    # Walk roots in a fixed order so re-runs don't reshuffle which capture
    # gets the canonical (un-hashed) filename.
    # Dedupe key: (vid, pid, device_name). If the same physical device's
    # capture appears in two comkits subfolders (e.g. someone re-probed it),
    # we only keep one — different captures of literally the same hardware
    # don't add coverage.
    seen: set[tuple[str, str, str]] = set()
    for root in SRC_ROOTS:
        if not root.exists():
            continue
        for toml in sorted(root.glob("kit_*.toml")):
            fields = parse_toml(toml)
            key = (
                hex4(fields.get("vendor_id", "0x0").strip('"')),
                hex4(fields.get("product_id", "0x0").strip('"')),
                fields.get("name", ""),
            )
            if key in seen:
                continue
            seen.add(key)
            import_one(toml, has_canonical)
    return 0


if __name__ == "__main__":
    sys.exit(main())
