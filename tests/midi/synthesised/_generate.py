#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Generator for the tests/midi/synthesised/*.midi.jsonl corpus.
# Reproducible: seeded RNG so the same outputs land every run; rerun
# with `python3 _generate.py` after changing this file and commit the
# results.
#
# The fixtures cover the failure modes the wizard / derive / runtime
# need to handle gracefully:
#   * different drum modules with different default MIDI note maps
#   * modules that send Note On + Note Off cleanly (TD-series style)
#   * modules that use velocity-0 Note On as their Note Off encoding
#   * modules that never send Note Off — wizard's per-pad velocity
#     decay window must kick in
#   * pads that report rim + head as two separate notes
#   * Pro kits with cymbal pads
#   * worn / low-velocity modules whose peak velocity is below ~100,
#     so the velocity_scaling logic actually has work to do

import json
import random
import sys
import uuid
from pathlib import Path


# Each profile names the device, its device_type (drum | pro_drum), and a
# map of step-name → list[int] of MIDI notes that pad emits. Notes
# beyond the first one in a list represent rim / head / variation
# triggers — the wizard's RankNotesForStep keeps the top few by peak
# velocity so multi-note pads work without special handling.
PROFILES = [
    {
        "id": "td1kv_clean",
        "device_name": "Roland TD-1KV",
        "port_id": "24:0",
        "device_type": "drum",
        "uses_note_off": True,
        "vel_range": (70, 127),
        "note_off_via_vel0": False,
        "pads": {
            "red_pad":    [38],
            "blue_pad":   [47],
            "yellow_pad": [48],
            "green_pad":  [41],
            "kick_pedal": [36],
        },
    },
    {
        "id": "alesis_nitro_no_note_off",
        "device_name": "Alesis Nitro Mesh",
        "port_id": "20:0",
        "device_type": "drum",
        # Some cheaper modules send Note On only, no Note Off. Wizard's
        # ~80 ms decay window has to clear the pad on its own.
        "uses_note_off": False,
        "vel_range": (50, 120),
        "note_off_via_vel0": False,
        "pads": {
            "red_pad":    [38],
            "blue_pad":   [45],
            "yellow_pad": [48],
            "green_pad":  [43],
            "kick_pedal": [36],
        },
    },
    {
        "id": "yamaha_dtx_vel0_off",
        "device_name": "Yamaha DTX402",
        "port_id": "28:1",
        "device_type": "drum",
        # DTX series often uses Note On with velocity 0 as the Note Off
        # encoding (running-status optimisation). Exercises that branch
        # in the wizard's drain loop.
        "uses_note_off": False,
        "note_off_via_vel0": True,
        "vel_range": (60, 115),
        "pads": {
            "red_pad":    [38],
            "blue_pad":   [47],
            "yellow_pad": [50],
            "green_pad":  [41],
            "kick_pedal": [36],
        },
    },
    {
        "id": "pearl_mimic_multi_note",
        "device_name": "Pearl Mimic Pro",
        "port_id": "30:0",
        "device_type": "drum",
        "uses_note_off": True,
        "vel_range": (40, 100),
        "note_off_via_vel0": False,
        # Mid-tier kits report rim + head as separate notes. The wizard
        # keeps the top few by peak velocity so both end up in the pad
        # map.
        "pads": {
            "red_pad":    [38, 40],          # snare head + rim
            "blue_pad":   [47, 46],
            "yellow_pad": [48, 50],
            "green_pad":  [41, 43],
            "kick_pedal": [36, 35],
        },
    },
    {
        "id": "pro_kit_with_cymbals",
        "device_name": "Roland TD-17 (Pro)",
        "port_id": "24:1",
        "device_type": "pro_drum",
        "uses_note_off": True,
        "vel_range": (65, 127),
        "note_off_via_vel0": False,
        "pads": {
            "red_pad":       [38],
            "blue_pad":      [47],
            "yellow_pad":    [48],
            "green_pad":     [41],
            "kick_pedal":    [36],
            "yellow_cymbal": [42, 46],       # hi-hat closed/open
            "blue_cymbal":   [49],           # crash
            "green_cymbal":  [51, 53],       # ride bow + bell
        },
    },
    {
        "id": "worn_pads_low_velocity",
        "device_name": "Worn-out Practice Kit",
        "port_id": "30:1",
        "device_type": "drum",
        "uses_note_off": True,
        # Peak velocity around 70 — velocity_scaling must compensate.
        "vel_range": (15, 75),
        "note_off_via_vel0": False,
        "pads": {
            "red_pad":    [38],
            "blue_pad":   [47],
            "yellow_pad": [48],
            "green_pad":  [41],
            "kick_pedal": [36],
        },
    },
]


def make_events(rng, notes, vel_range, uses_note_off, note_off_via_vel0, t0):
    """Build a list of MidiEvent dicts for one step.

    Each hit is 2-12 events: pick a note from the pad's note list,
    Note On with random velocity, Note Off ~30-60 ms later (or
    nothing, depending on profile).
    """
    n_hits = rng.randint(3, 8)
    out = []
    t = t0 + rng.randint(10, 40)
    for _ in range(n_hits):
        note = rng.choice(notes)
        vel = rng.randint(*vel_range)
        out.append({"on": True, "note": note, "vel": vel, "t_ms": t})
        gap = rng.randint(25, 65)
        if uses_note_off:
            out.append({"on": False, "note": note, "vel": 0, "t_ms": t + gap})
        elif note_off_via_vel0:
            out.append({"on": True, "note": note, "vel": 0, "t_ms": t + gap})
        # else: module sends no Note Off at all (wizard's decay handles it)
        # Move to the next hit, anywhere 50-350 ms later.
        t = t + gap + rng.randint(120, 350)
    return out


def write_profile(profile, out_dir, seed):
    rng = random.Random(seed + hash(profile["id"]) & 0xFFFFFFFF)
    # Pin the UUID + timestamp so the file is byte-stable across runs.
    capture_uuid = str(uuid.UUID(int=rng.getrandbits(128)))
    host_hash = "0000000000000000"
    timestamp = "2026-06-06T12:00:00Z"

    lines = []
    meta = {
        "type": "meta",
        "schema": "shadps4-midi-instrument/v1",
        "version": 1,
        "device_name": profile["device_name"],
        "port_id": profile["port_id"],
        "source": "midi",
        "device_type": profile["device_type"],
        "timestamp": timestamp,
        "capture_uuid": capture_uuid,
        "host_hash": host_hash,
    }
    lines.append(json.dumps(meta, separators=(",", ":")))
    lines.append(json.dumps({"step": "_idle_baseline", "events": []},
                            separators=(",", ":")))

    for step, notes in profile["pads"].items():
        events = make_events(
            rng, notes, profile["vel_range"],
            profile["uses_note_off"], profile["note_off_via_vel0"],
            t0=0,
        )
        lines.append(json.dumps({"step": step, "events": events},
                                separators=(",", ":")))

    out_path = out_dir / f"{profile['id']}.midi.jsonl"
    out_path.write_text("\n".join(lines) + "\n")
    print(f"wrote {out_path.relative_to(out_dir.parent.parent.parent)}")


def main():
    seed = 42
    if len(sys.argv) > 1:
        seed = int(sys.argv[1])
    out_dir = Path(__file__).resolve().parent
    for profile in PROFILES:
        write_profile(profile, out_dir, seed)


if __name__ == "__main__":
    main()
