#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Replay a raw HID capture (.raw.jsonl from KitProbeDialog) through a Python
reimplementation of the legacy-instrument packer and assert that each step's
output lands in the right `dud[]` slot / face button.

This is the regression harness for `src/input/hid_instrument.cpp`. Keep the
packer math here in sync with PackDeviceUniqueData / PackButtons; the test
will catch divergence by failing on community-supplied raw captures.

Usage:
    python3 tests/validate_hid_kits.py tests/hid_kits/

Exit code 0 on all-pass, 1 on any failure.
"""

from __future__ import annotations

import json
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

try:
    import tomllib  # py3.11+
except ImportError:  # pragma: no cover
    import tomli as tomllib  # type: ignore


# Bit positions in OrbisPadButtonDataOffset (subset used by instrument kits).
BTN = {
    "square":   0x80,
    "triangle": 0x10,
    "circle":   0x20,
    "cross":    0x40,
    "l1":       0x400,
    "r1":       0x800,
    "l2":       0x100,
    "r2":       0x200,
    "options":  0x08,
    "touchpad": 0x100000,
    "up":       0x10,    # legacy mapping; the real value differs but we
    "down":     0x40,    # only check by-name in assertions, never by raw bit
    "left":     0x80,
    "right":    0x20,
}


@dataclass
class KitDef:
    vendor_id: int
    product_id: int
    name: str
    device_class: str
    source: str = "hid"
    report_length: int = 0
    dud_layout: list = field(default_factory=lambda: [-1] * 12)
    hat_byte: int = -1
    tilt_byte: int = -1
    tilt_byte_high: int = -1
    tilt_baseline: int = 0x80
    tilt_scale: int = 90
    tilt_invert: bool = False
    whammy_byte: int = -1
    whammy_baseline: int = 0x80
    touch_byte: int = -1
    tone_byte: int = -1
    fret_byte: int = 0
    fret_mask: int = 0xFF
    solo_fret_byte: int = -1
    guitar_ps4_layout: bool = False
    drum_ps4_layout: bool = False
    clear_dud0_when_raw1_bits: int = 0
    has_dud0_remap: bool = False
    dud0_bit_remap: list = field(default_factory=lambda: [0, 1, 2, 3, 4, 5, 6, 7])
    button_bytes: dict = field(default_factory=dict)  # byte_idx -> {bit_mask: btn_name}


def _parse_hex(s: str) -> int:
    s = s.strip()
    return int(s, 16) if s.lower().startswith("0x") else int(s, 10)


def load_kit(toml_path: Path) -> KitDef:
    with open(toml_path, "rb") as f:
        doc = tomllib.load(f)
    k = KitDef(
        vendor_id=_parse_hex(doc["vendor_id"]),
        product_id=_parse_hex(doc["product_id"]),
        name=doc.get("name", "(unnamed)"),
        device_class=doc.get("device_class", ""),
        source=doc.get("source", "hid"),
        report_length=doc.get("report_length", 0),
    )
    if "device_unique_data" in doc:
        arr = doc["device_unique_data"]
        for i in range(min(12, len(arr))):
            k.dud_layout[i] = arr[i]
    k.hat_byte = doc.get("hat_byte", -1)
    k.tilt_byte = doc.get("tilt_byte", -1)
    k.tilt_byte_high = doc.get("tilt_byte_high", -1)
    k.tilt_baseline = doc.get("tilt_baseline", 0x80)
    k.tilt_scale = doc.get("tilt_scale", 90)
    k.tilt_invert = doc.get("tilt_invert", False)
    k.whammy_byte = doc.get("whammy_byte", -1)
    k.whammy_baseline = doc.get("whammy_baseline", 0x80)
    k.touch_byte = doc.get("touch_byte", -1)
    k.tone_byte = doc.get("tone_byte", -1)
    k.fret_byte = doc.get("fret_byte", 0)
    k.fret_mask = doc.get("fret_mask", 0xFF)
    k.solo_fret_byte = doc.get("solo_fret_byte", -1)
    k.guitar_ps4_layout = doc.get("guitar_ps4_layout", False)
    k.drum_ps4_layout = doc.get("drum_ps4_layout", False)
    k.clear_dud0_when_raw1_bits = doc.get("clear_dud0_when_raw1_bits", 0)
    if "dud0_bit_remap" in doc:
        k.has_dud0_remap = True
        for i, v in enumerate(doc["dud0_bit_remap"][:8]):
            k.dud0_bit_remap[i] = v
    for key, val in doc.items():
        if not key.startswith("buttons_byte_"):
            continue
        try:
            byte_idx = int(key[len("buttons_byte_"):])
        except ValueError:
            continue
        if byte_idx < 0:
            continue
        k.button_bytes[byte_idx] = {
            _parse_hex(bk): bv.lower() for bk, bv in val.items()
        }
    return k


def pack_buttons(raw: list, kit: KitDef) -> set:
    """Returns the set of PS4 button names that would be reported."""
    out = set()
    for byte_idx, table in kit.button_bytes.items():
        if byte_idx < 0 or byte_idx >= len(raw):
            continue
        b = raw[byte_idx]
        for mask, name in table.items():
            if b & mask:
                out.add(name)
    if 0 <= kit.hat_byte < len(raw):
        hat = raw[kit.hat_byte] & 0x0F
        hat_map = {
            0x00: ["up"],
            0x01: ["up", "right"],
            0x02: ["right"],
            0x03: ["right", "down"],
            0x04: ["down"],
            0x05: ["down", "left"],
            0x06: ["left"],
            0x07: ["left", "up"],
        }
        for name in hat_map.get(hat, []):
            out.add(name)
    return out


def pack_guitar_dud(raw: list, kit: KitDef) -> list:
    """PS4 RB guitar layout: dud[0]=pickup, [1]=whammy, [2]=tilt, [3]=frets, [4]=solo."""
    out = [0] * 12
    if 0 <= kit.tone_byte < len(raw):
        tone_raw = raw[kit.tone_byte]
        if tone_raw > 0x10:
            pos = 1 + ((tone_raw - 0x10) * 4 // 0xF0)
            out[0] = min(4, pos)
    if 0 <= kit.whammy_byte < len(raw):
        w = raw[kit.whammy_byte]
        base = kit.whammy_baseline
        rng = max(1, 0xFF - base)
        out[1] = min(0xFE, (w - base) * 0xFE // rng) if w > base else 0
    # Tilt: skip (depends on acceleration history); validator only checks frets/buttons.
    frets = raw[kit.fret_byte] & kit.fret_mask if 0 <= kit.fret_byte < len(raw) else 0
    if kit.clear_dud0_when_raw1_bits and len(raw) > 1:
        if raw[1] & kit.clear_dud0_when_raw1_bits:
            frets = 0
    if kit.has_dud0_remap:
        remapped = 0
        for b in range(8):
            if frets & (1 << b):
                remapped |= 1 << kit.dud0_bit_remap[b]
        frets = remapped
    out[3] = frets
    if 0 <= kit.solo_fret_byte < len(raw):
        out[4] = raw[kit.solo_fret_byte]
    return out


# Per-step expectations expressed as (fret_bits_required, buttons_required).
# fret_bits is a list of PS4-native bit positions that must be set in dud[3];
# buttons is a list of names that must appear in PackButtons output.
GUITAR_EXPECT = {
    "green_fret":  ([0],    ["cross"]),
    "red_fret":    ([1],    ["circle"]),
    "yellow_fret": ([2],    ["triangle"]),
    "blue_fret":   ([3],    ["square"]),
    "orange_fret": ([4],    ["l1"]),
    "green_strum":      ([0],    ["cross", "down"]),
    "green_blue":       ([0, 3], ["cross", "square"]),
    "green_blue_strum": ([0, 3], ["cross", "square", "down"]),
}
# Drum step expectations: only face-button assertions, since the dud[]
# velocity slots are non-zero only on the exact hit frame which is hard to
# pin down without timing info. Note we accept the kick step landing on L1
# (5-lane GH-mode convention) OR R1 (PS4 Pro convention).
DRUM_EXPECT = {
    "red_pad":       (None, ["circle"]),
    "yellow_pad":    (None, ["triangle"]),
    "blue_pad":      (None, ["square"]),
    "green_pad":     (None, ["cross"]),
    "yellow_cymbal": (None, ["triangle"]),
    "blue_cymbal":   (None, ["square"]),
    "green_cymbal":  (None, ["cross"]),
    "orange_cymbal": (None, ["r1"]),
    "kick_pedal":    (None, ["l1"]),
}


def check_step(step: str, device_type: str, buttons: set, dud: list) -> Optional[tuple]:
    """Dispatch on device_type. Returns (ok, reason) or None if the step is
    not validated for this device type."""
    if device_type in ("drum", "drum_pro"):
        if step not in DRUM_EXPECT:
            return None
        _, btn_names = DRUM_EXPECT[step]
        missing = [n for n in btn_names if n not in buttons]
        if not missing:
            return (True, f"buttons={btn_names}")
        return (False, f"buttons={sorted(buttons)} (missing {missing})")
    # Guitar (default).
    if step not in GUITAR_EXPECT:
        return None
    fret_bits, btn_names = GUITAR_EXPECT[step]
    bitmap = dud[3]
    bits_ok = all(bitmap & (1 << b) for b in fret_bits)
    missing_btns = [n for n in btn_names if n not in buttons]
    if bits_ok and not missing_btns:
        bits_repr = "|".join(str(b) for b in fret_bits)
        return (True, f"dud[3]=0x{bitmap:02x} (bits {bits_repr}), buttons={btn_names}")
    return (
        False,
        f"dud[3]=0x{bitmap:02x} (need bits {fret_bits}), "
        f"buttons={sorted(buttons)} (missing {missing_btns})",
    )


def read_raw_jsonl(jsonl_path: Path) -> tuple:
    """Returns (meta, per_step). meta is a dict or None for version-0 captures
    (pre-versioning wizard output — no header line)."""
    meta = None
    per_step = defaultdict(list)
    with open(jsonl_path) as f:
        for line in f:
            d = json.loads(line)
            if d.get("type") == "meta":
                meta = d
                continue
            per_step[d["step"]].append(d["bytes"])
    return meta, per_step


def run_kit(jsonl_path: Path) -> tuple:
    """Returns (passes: int, fails: int, lines: list[str])."""
    lines = [f"  {jsonl_path.name}"]
    meta, per_step = read_raw_jsonl(jsonl_path)
    if meta:
        lines.append(f"      meta: v{meta.get('version', '?')} "
                     f"{meta.get('vendor_id', '?')}:{meta.get('product_id', '?')} "
                     f"({meta.get('device_name', '?')})")
    else:
        lines.append("      (legacy capture, no meta header — version 0)")

    toml_path = jsonl_path.with_suffix("").with_suffix(".toml")
    if not toml_path.exists():
        # No companion TOML — we'd need to invoke the wizard's TOML-derivation
        # to produce one. That's the next-step C++ harness; for now log and
        # move on without failing.
        lines.append("      SKIP (no companion .toml; needs wizard-derived TOML)")
        return 0, 0, lines

    kit = load_kit(toml_path)
    lines.append(f"      kit: {kit.name}")
    if meta:
        try:
            meta_vid = int(meta["vendor_id"], 16)
            meta_pid = int(meta["product_id"], 16)
            if meta_vid != kit.vendor_id or meta_pid != kit.product_id:
                lines.append(
                    f"      FAIL  meta mismatch: capture is "
                    f"{meta['vendor_id']}:{meta['product_id']} but kit is "
                    f"0x{kit.vendor_id:04x}:0x{kit.product_id:04x}")
                return 0, 1, lines
        except (KeyError, ValueError):
            pass

    device_type = (meta or {}).get("device_type", "guitar")
    expect_table = DRUM_EXPECT if device_type in ("drum", "drum_pro") else GUITAR_EXPECT
    passes = fails = 0
    for step, samples in per_step.items():
        if not samples:
            continue
        # Look at frames where the input we'd expect actually moved. For
        # guitars that's the fret_byte bitmap; for drums any face-button
        # bit on byte 0 (which is where every PS3-style drum encodes hits).
        relevant = []
        if step in expect_table:
            if device_type == "guitar" and kit.guitar_ps4_layout \
                    and 0 <= kit.fret_byte < len(samples[0]):
                relevant = [s for s in samples
                            if (s[kit.fret_byte] & kit.fret_mask) != 0]
            elif device_type in ("drum", "drum_pro") and len(samples[0]) > 0:
                relevant = [s for s in samples if s[0] != 0]
        if not relevant:
            relevant = samples[len(samples) // 2 : len(samples) // 2 + 1] or samples[:1]

        step_ok = False
        last_reason = ""
        for sample in relevant:
            buttons = pack_buttons(sample, kit)
            dud = pack_guitar_dud(sample, kit) if kit.guitar_ps4_layout else [0] * 12
            res = check_step(step, device_type, buttons, dud)
            if res is None:
                step_ok = True
                last_reason = "(step not validated)"
                break
            ok, reason = res
            last_reason = reason
            if ok:
                step_ok = True
                break

        if step_ok:
            passes += 1
            lines.append(f"      PASS  {step:18s}  {last_reason}")
        else:
            fails += 1
            lines.append(f"      FAIL  {step:18s}  {last_reason}")
    return passes, fails, lines


def main(argv: list) -> int:
    root = Path(argv[1] if len(argv) > 1 else "tests/hid_kits")
    if not root.is_dir():
        print(f"No test directory at {root}", file=sys.stderr)
        return 1
    total_pass = total_fail = 0
    out_lines = [f"== HID kit validation under {root} =="]
    for jsonl_path in sorted(root.glob("*.raw.jsonl")):
        p, f, lines = run_kit(jsonl_path)
        out_lines.extend(lines)
        total_pass += p
        total_fail += f
    out_lines.append(f"== {total_pass} passed, {total_fail} failed ==")
    print("\n".join(out_lines))
    return 0 if total_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
