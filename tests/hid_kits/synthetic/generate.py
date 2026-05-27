#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Generate synthetic raw-HID .raw.jsonl fixtures for every PlasticBand kit
class shadPS4 claims to support. Each fixture mirrors what KitProbeDialog
would capture from real hardware: ~30 baseline frames, then per-step
multi-frame press/release patterns. Wizard heuristics need transitions
(byte changing from baseline to a non-baseline value across frames), so
each step writes "released → pressed → released" rather than a single
sample.

Run from anywhere:
    python3 tests/hid_kits/synthetic/generate.py

Byte layouts come from PlasticBand specs (fetched verbatim, not memorised):
- 5-Fret Guitar/Rock Band/{PS3,PS4,PS5,Xbox 360}.md
- 4-Lane Drums/{PS3 and Wii,PS4,Xbox 360}.md
- 5-Lane Drums/{PS3,Xbox 360,Santroller}.md
Keep this script in sync when a spec changes upstream.

The hidtest harness then asserts:
  - DeriveKitToml emits the right fields for the device class.
  - PackButtons / PackDeviceUniqueData produce at least one non-zero
    output bit per step.
"""

import json
import sys
from pathlib import Path

OUTDIR = Path(__file__).resolve().parent


# ---------------------------------------------------------------------------
# Frame builder: collects baseline frames, then per-step natural patterns.
# Each "captured" step emits N "released" frames, then N "pressed" frames,
# then N more "released" frames, alternating a few times. The wizard's
# detectAllFlagCandidates / velByte heuristics need r.bytes[i].max to
# exceed baseline_max[i], so a single non-zero frame is enough — but the
# multi-frame pattern makes the data look like a real capture, exercises
# transition counts, and lets the round-trip stage prove a transition
# fired between baseline and the press.
# ---------------------------------------------------------------------------
SYNTHETIC_VERSION = 3  # combo + solo-fret + orange_pad capture coverage


class Kit:
    def __init__(self, name: str, meta: dict, idle: list[int]):
        meta.setdefault("version", SYNTHETIC_VERSION)
        self.name = name
        self.meta = meta
        self.idle = list(idle)
        self.frames: list[tuple[str, list[int]]] = []
        # Sanity check vs. the meta's report_length.
        assert len(idle) == meta["report_length"], \
            f"{name}: idle len {len(idle)} != report_length {meta['report_length']}"
        for _ in range(30):
            self.frames.append(("_motion_baseline", list(idle)))

    def _press_cycle(self, key: str, mutate, n_cycles: int = 3, samples_per_phase: int = 4):
        """Emit `n_cycles` press/release cycles. `mutate(frame, pressed)` writes
        whatever bytes should change for this step into `frame` when pressed=True
        (and leaves them at baseline when pressed=False). The wizard sees the
        pressed-state frames as "this byte/bit varied during step X"."""
        for _ in range(n_cycles):
            for pressed in (False, True, True, False):
                _ = pressed  # placeholder; phase is determined by index
            for phase in range(samples_per_phase * 2):
                pressed = phase >= samples_per_phase
                frame = list(self.idle)
                mutate(frame, pressed)
                self.frames.append((key, frame))

    def step_bits(self, key: str, byte: int, mask: int) -> None:
        """Digital step: bit `mask` in `byte` toggles on and off."""
        def m(f, pressed):
            if pressed:
                f[byte] |= mask
        self._press_cycle(key, m)

    def step_set_byte(self, key: str, byte: int, value: int) -> None:
        """Digital step that writes a fixed byte value when pressed (e.g. HAT)."""
        def m(f, pressed):
            if pressed:
                f[byte] = value
        self._press_cycle(key, m)

    def step_velocity(self, key: str, byte: int, peak: int = 0x7F,
                       baseline: int = 0) -> None:
        """Analog/velocity step: byte ramps from baseline up to `peak` and back."""
        # Manually emit a ramp so the wizard sees a real velocity curve.
        ramp = [baseline] + list(range(0x10, peak + 1, max(1, (peak - 0x10) // 6))) \
               + [peak, peak, peak] \
               + list(range(peak, baseline, -max(1, (peak - 0x10) // 6))) \
               + [baseline] * 3
        for v in ramp:
            frame = list(self.idle)
            frame[byte] = v
            self.frames.append((key, frame))

    def step_axis(self, key: str, byte: int, peak: int, center: int = 0x80) -> None:
        """Analog axis step (e.g. tilt). Pushes byte from center to peak."""
        ramp = [center, center, center] \
               + list(range(center, peak, (peak - center) // 6 or 1)) \
               + [peak, peak, peak] \
               + list(range(peak, center, -((peak - center) // 6 or 1))) \
               + [center, center, center]
        for v in ramp:
            frame = list(self.idle)
            frame[byte] = v
            self.frames.append((key, frame))

    def step_combo(self, key: str, mutate) -> None:
        """Combo step (multiple bytes change at once). `mutate(frame)` is called
        on every pressed-phase frame. The combo step exists so the wizard can
        validate fret-byte detection: e.g. green+blue both pressed must show
        two bits set in the same byte, not separate bytes."""
        for _ in range(3):
            for _ in range(4):
                self.frames.append((key, list(self.idle)))
            for _ in range(4):
                frame = list(self.idle)
                mutate(frame)
                self.frames.append((key, frame))
            for _ in range(4):
                self.frames.append((key, list(self.idle)))

    def write(self) -> None:
        path = OUTDIR / self.name
        sep = (",", ":")
        with path.open("w") as f:
            f.write(json.dumps({"type": "meta", **self.meta}, separators=sep) + "\n")
            for step, raw in self.frames:
                f.write(json.dumps({"step": step, "bytes": raw}, separators=sep) + "\n")
        print(f"wrote {path.name} ({len(self.frames)} frames)")


# ---------------------------------------------------------------------------
# Per-device generators — byte layouts come from PlasticBand specs.
# Each generator covers every step the wizard's walkthrough records.
# Generators do not invent byte layouts: refer to the comment block at the
# top of each function for the spec source.
# ---------------------------------------------------------------------------

# Spec: PlasticBand 5-Fret Guitar/Rock Band/PS4.md
#   Report length 64. fret_byte=46, solo_fret_byte=47, hat=byte 5 lo nibble,
#   whammy byte 44, tilt byte 45, tone byte 43.
#   Face-flag byte 5 upper nibble: blue/green/red/yellow.
#   Byte 6: orange (bit 0), share (bit 4), options (bit 5), solo flag (bit 6),
#   P1 (bit 7). Byte 7 bit 0 = PS button.
def gen_ps4_rb_mustang() -> None:
    rep_len = 64
    idle = [0] * rep_len
    idle[1] = idle[2] = 0x80
    idle[5] = 0x08              # HAT neutral
    k = Kit(
        "ps4_rb_mustang.raw.jsonl",
        {"vendor_id": "0x0e6f", "product_id": "0x024a",
         "device_name": "PS4 RB Mustang (synthetic)",
         "device_type": "guitar_solo", "source": "hid",
         "report_length": rep_len},
        idle,
    )
    # tilt: byte 45 ramps up
    k.step_axis("tilt_up", 45, 0xE0, center=0x00)
    # main frets on byte 46
    for bit, key in enumerate(("green_fret", "red_fret", "yellow_fret",
                                "blue_fret", "orange_fret")):
        k.step_bits(key, 46, 1 << bit)
    # solo frets on byte 47
    for bit, key in enumerate(("solo_green_fret", "solo_red_fret",
                                "solo_yellow_fret", "solo_blue_fret",
                                "solo_orange_fret")):
        k.step_bits(key, 47, 1 << bit)
    # strum / dpad on byte 5 lower nibble (HID HAT)
    for key, hat in (("strum_up", 0x00), ("strum_down", 0x04),
                     ("dpad_up", 0x00), ("dpad_down", 0x04),
                     ("dpad_left", 0x06), ("dpad_right", 0x02)):
        k.step_set_byte(key, 5, hat)
    # combo: green held + strum down (frets byte 46, hat byte 5 lo nibble)
    def green_strum(f):
        f[46] |= 0x01
        f[5] = 0x04
    k.step_combo("green_strum", green_strum)
    def green_blue(f):
        f[46] |= 0x01 | 0x08
    k.step_combo("green_blue", green_blue)
    def green_blue_strum(f):
        f[46] |= 0x01 | 0x08
        f[5] = 0x04
    k.step_combo("green_blue_strum", green_blue_strum)
    # whammy: byte 44 ramps 0→0xFF
    k.step_velocity("whammy_bar", 44, peak=0xFF, baseline=0x00)
    # touch / tone slider: byte 43 takes positions 0x10..0xFF
    k.step_velocity("touch_slider", 43, peak=0xE0, baseline=0x00)
    # menu buttons
    k.step_bits("button_start",  6, 0x20)
    k.step_bits("button_select", 6, 0x10)
    k.step_bits("button_ps",     7, 0x01)
    k.write()


# Spec: PlasticBand 5-Fret Guitar/Rock Band/PS5.md
#   Report length 64. fret_byte=43, solo_fret_byte=44, hat=byte 8 lo nibble,
#   whammy byte 41, tilt byte 42 (0x00 parallel, 0xFF upright → invert).
#   Byte 8 upper nibble carries face-flag fret bits; byte 9 carries
#   orange/share/options/solo/P1. Byte 10 bit 0 = PS button.
def gen_ps5_riffmaster() -> None:
    rep_len = 64
    idle = [0] * rep_len
    idle[1] = idle[2] = 0x80
    idle[8] = 0x08              # HAT neutral
    idle[42] = 0x10             # tilt rests near 0; raw goes UP on tilt-up
    k = Kit(
        "ps5_riffmaster.raw.jsonl",
        {"vendor_id": "0x0e6f", "product_id": "0x0249",
         "device_name": "PS5 Riffmaster (synthetic)",
         "device_type": "guitar_solo", "source": "hid",
         "report_length": rep_len},
        idle,
    )
    k.step_axis("tilt_up", 42, 0xF0, center=0x10)
    for bit, key in enumerate(("green_fret", "red_fret", "yellow_fret",
                                "blue_fret", "orange_fret")):
        k.step_bits(key, 43, 1 << bit)
    for bit, key in enumerate(("solo_green_fret", "solo_red_fret",
                                "solo_yellow_fret", "solo_blue_fret",
                                "solo_orange_fret")):
        k.step_bits(key, 44, 1 << bit)
    for key, hat in (("strum_up", 0x00), ("strum_down", 0x04),
                     ("dpad_up", 0x00), ("dpad_down", 0x04),
                     ("dpad_left", 0x06), ("dpad_right", 0x02)):
        k.step_set_byte(key, 8, hat)
    def green_strum(f):
        f[43] |= 0x01
        f[8] = 0x04
    k.step_combo("green_strum", green_strum)
    def green_blue(f):
        f[43] |= 0x01 | 0x08
    k.step_combo("green_blue", green_blue)
    def green_blue_strum(f):
        f[43] |= 0x01 | 0x08
        f[8] = 0x04
    k.step_combo("green_blue_strum", green_blue_strum)
    k.step_velocity("whammy_bar", 41, peak=0xFF, baseline=0x00)
    # Riffmaster has no touch slider — skip.
    k.step_bits("button_start",  9, 0x20)
    k.step_bits("button_select", 9, 0x10)
    k.step_bits("button_ps",    10, 0x01)
    k.write()


# Spec: PlasticBand 5-Fret Guitar/Rock Band/PS3.md (mirrored conventions from
# the PS3 GH/RB family — 27-byte HID report, fret bits on byte 0, HAT on
# byte 2, face-button byte on byte 1, whammy on byte 6, accel.x split
# across bytes 19/20 in PS3 layout).
def gen_ps3_rb_guitar() -> None:
    rep_len = 27
    idle = [0] * rep_len
    idle[2] = 0x08              # HAT neutral
    idle[3] = idle[4] = idle[5] = idle[6] = 0x80
    idle[19] = 0x00             # accel low byte
    idle[20] = 0x02             # accel high byte (~512 center for 10-bit)
    k = Kit(
        "ps3_rb_guitar.raw.jsonl",
        {"vendor_id": "0x12ba", "product_id": "0x0200",
         "device_name": "PS3 RB Guitar (synthetic)",
         "device_type": "guitar", "source": "hid",
         "report_length": rep_len},
        idle,
    )
    # tilt: PS3 accel.x is 10-bit across bytes 19 (low) and 20 (high 2 bits).
    # Tilt up DROPS the raw → step_axis with peak below center.
    k.step_axis("tilt_up", 19, 0x40, center=0x00)
    # frets in byte 0; PS3 has non-standard bit order (blue=0, green=1, red=2,
    # yellow=3, orange=4).
    for bit, key in (
            (0, "blue_fret"), (1, "green_fret"), (2, "red_fret"),
            (3, "yellow_fret"), (4, "orange_fret")):
        k.step_bits(key, 0, 1 << bit)
    for key, hat in (("strum_up", 0x00), ("strum_down", 0x04),
                     ("dpad_up", 0x00), ("dpad_down", 0x04),
                     ("dpad_left", 0x06), ("dpad_right", 0x02)):
        k.step_set_byte(key, 2, hat)
    def green_strum(f):
        f[0] |= 0x02
        f[2] = 0x04
    k.step_combo("green_strum", green_strum)
    def green_blue(f):
        f[0] |= 0x02 | 0x01
    k.step_combo("green_blue", green_blue)
    def green_blue_strum(f):
        f[0] |= 0x02 | 0x01
        f[2] = 0x04
    k.step_combo("green_blue_strum", green_blue_strum)
    k.step_velocity("whammy_bar", 6, peak=0xFF, baseline=0x80)
    k.step_bits("button_start",  1, 0x04)
    k.step_bits("button_select", 1, 0x02)
    k.step_bits("button_ps",     1, 0x10)
    k.write()


# Spec: PlasticBand 5-Fret Guitar/Rock Band/Xbox 360.md
#   XInput; 17-byte synthetic report from FillXInputReport.
#   frets: A=green(bit0), B=red(bit1), Y=blue(bit3), X=yellow(bit2),
#   LB=orange(bit4); strum on byte 2 d-pad bitmap; whammy on byte 5 (RX u8);
#   tilt on byte 6 (RY u8); pickup on byte 7 (LT u8).
def gen_x360_rb_guitar() -> None:
    rep_len = 17
    idle = [0] * rep_len
    idle[3] = idle[4] = 0x80
    idle[5] = idle[6] = 0x80
    k = Kit(
        "x360_rb_guitar.raw.jsonl",
        {"vendor_id": "0x1bad", "product_id": "0x0004",
         "device_name": "Xbox 360 RB Guitar (synthetic)",
         "device_type": "guitar", "source": "xinput",
         "report_length": rep_len},
        idle,
    )
    k.step_axis("tilt_up", 6, 0xE0, center=0x80)
    for bit, key in (
            (0, "green_fret"), (1, "red_fret"), (3, "blue_fret"),
            (2, "yellow_fret"), (4, "orange_fret")):
        k.step_bits(key, 0, 1 << bit)
    # XInput byte 2 is a 4-bit dpad bitmap, NOT an HID HAT.
    for key, bit in (("strum_up", 0), ("strum_down", 1),
                     ("dpad_up", 0), ("dpad_down", 1),
                     ("dpad_left", 2), ("dpad_right", 3)):
        k.step_bits(key, 2, 1 << bit)
    def green_strum(f):
        f[0] |= 0x01
        f[2] |= 0x02  # strum down
    k.step_combo("green_strum", green_strum)
    def green_blue(f):
        f[0] |= 0x01 | 0x08
    k.step_combo("green_blue", green_blue)
    def green_blue_strum(f):
        f[0] |= 0x01 | 0x08
        f[2] |= 0x02
    k.step_combo("green_blue_strum", green_blue_strum)
    k.step_velocity("whammy_bar", 5, peak=0xFF, baseline=0x80)
    k.step_velocity("touch_slider", 7, peak=0xFF, baseline=0x00)  # via LT
    k.step_bits("button_start",  1, 0x01)
    k.step_bits("button_select", 1, 0x02)
    k.step_bits("button_ps",     1, 0x04)
    k.write()


# Spec: PlasticBand 5-Fret Guitar/Guitar Hero/Xbox 360.md (GH X360 variant).
#   Same XInput byte map as RB but with GH fret colour-to-button mapping
#   (yellow on Y / blue on X — reversed from RB) and a touch slider on LY.
def gen_x360_gh_guitar() -> None:
    rep_len = 17
    idle = [0] * rep_len
    idle[3] = idle[4] = 0x80
    idle[5] = idle[6] = 0x80
    k = Kit(
        "x360_gh_guitar.raw.jsonl",
        {"vendor_id": "0x1430", "product_id": "0x474b",
         "device_name": "Xbox 360 GH Guitar (synthetic)",
         "device_type": "guitar", "source": "xinput",
         "report_length": rep_len},
        idle,
    )
    k.step_axis("tilt_up", 6, 0xE0, center=0x80)
    for bit, key in (
            (0, "green_fret"), (1, "red_fret"), (3, "yellow_fret"),
            (2, "blue_fret"), (4, "orange_fret")):
        k.step_bits(key, 0, 1 << bit)
    for key, bit in (("strum_up", 0), ("strum_down", 1),
                     ("dpad_up", 0), ("dpad_down", 1),
                     ("dpad_left", 2), ("dpad_right", 3)):
        k.step_bits(key, 2, 1 << bit)
    def green_strum(f):
        f[0] |= 0x01
        f[2] |= 0x02
    k.step_combo("green_strum", green_strum)
    def green_blue(f):
        f[0] |= 0x01 | 0x04
    k.step_combo("green_blue", green_blue)
    def green_blue_strum(f):
        f[0] |= 0x01 | 0x04
        f[2] |= 0x02
    k.step_combo("green_blue_strum", green_blue_strum)
    k.step_velocity("whammy_bar", 5, peak=0xFF, baseline=0x80)
    k.step_velocity("touch_slider", 4, peak=0xFF, baseline=0x80)  # LY axis
    k.step_bits("button_start",  1, 0x01)
    k.step_bits("button_select", 1, 0x02)
    k.step_bits("button_ps",     1, 0x04)
    k.write()


# Spec: PlasticBand 4-Lane Drums/PS4.md
#   PS4 RB Pro drums. dud[0..6] wire bytes carry red/blue/yellow/green pad
#   velocities + yellow/blue/green cymbal velocities. Wizard learns each
#   pad's source byte from per-pad velocity steps.
def gen_ps4_rb_drums_pro() -> None:
    rep_len = 32
    idle = [0] * rep_len
    idle[5] = 0x08              # HAT neutral
    k = Kit(
        "ps4_rb_drums_pro.raw.jsonl",
        {"vendor_id": "0x0e6f", "product_id": "0x0173",
         "device_name": "PS4 RB Pro Drums (synthetic)",
         "device_type": "drum_pro", "source": "hid",
         "report_length": rep_len},
        idle,
    )
    # Per-pad / per-cymbal velocities — distinct bytes so the wizard can
    # tell them apart, with the digital flag bits set on byte 0 too (real
    # drum kits drive both at once).
    pads = [
        ("red_pad",       11, 0, 0x20),   # red drum vel @ 11, circle bit
        ("blue_pad",      12, 0, 0x10),   # square
        ("yellow_pad",    13, 0, 0x80),   # triangle
        ("green_pad",     14, 0, 0x40),   # cross
        ("yellow_cymbal", 15, 0, 0x80),
        ("blue_cymbal",   16, 0, 0x10),
        ("green_cymbal",  17, 0, 0x40),
    ]
    for key, vbyte, _fbyte, fmask in pads:
        # Velocity ramp + face-flag bit while pressed.
        def make_mutator(vb, fm):
            def m(f, pressed):
                if pressed:
                    f[vb] = 0x7F
                    f[0] |= fm
            return m
        k._press_cycle(key, make_mutator(vbyte, fmask))
    k.step_bits("kick_pedal", 0, 0x01)
    k.step_bits("kick_pedal_2", 0, 0x02)
    k.step_bits("button_cross",    0, 0x40)
    k.step_bits("button_circle",   0, 0x20)
    k.step_bits("button_square",   0, 0x10)
    k.step_bits("button_triangle", 0, 0x80)
    k.step_bits("button_start",  6, 0x20)
    k.step_bits("button_select", 6, 0x10)
    k.step_bits("button_ps",     7, 0x01)
    for key, hat in (("dpad_up", 0x00), ("dpad_down", 0x04),
                     ("dpad_left", 0x06), ("dpad_right", 0x02)):
        k.step_set_byte(key, 5, hat)
    k.write()


# Spec: PlasticBand 4-Lane Drums/PS3 and Wii.md
#   21-byte report. Pads: button bits in byte 1 (Sq=blue, Cr=green, Ci=red,
#   Tr=yellow) plus L3 (pad flag) / R3 (cymbal flag) in byte 2. Kick = L1.
#   Velocities at bytes 11..14: 11=yellow, 12=red, 13=green, 14=blue.
def gen_ps3_rb_drums() -> None:
    rep_len = 21
    idle = [0] * rep_len
    idle[3] = 0x08
    k = Kit(
        "ps3_rb_drums.raw.jsonl",
        {"vendor_id": "0x12ba", "product_id": "0x0210",
         "device_name": "PS3 RB Drums (synthetic)",
         "device_type": "drum", "source": "hid",
         "report_length": rep_len},
        idle,
    )
    def pad(face_bit: int, vel_byte: int, key: str):
        def m(f, pressed):
            if pressed:
                f[1] |= face_bit
                f[2] |= 0x04  # L3 pad flag
                f[vel_byte] = 0x7F
        k._press_cycle(key, m)

    pad(0x04, 12, "red_pad")
    pad(0x01, 14, "blue_pad")
    pad(0x08, 11, "yellow_pad")
    pad(0x02, 13, "green_pad")
    k.step_bits("kick_pedal",  1, 0x10)
    k.step_bits("kick_pedal_2", 1, 0x20)
    # Face buttons (without pad flag) so they don't masquerade as pad hits.
    k.step_bits("button_square",   1, 0x01)
    k.step_bits("button_cross",    1, 0x02)
    k.step_bits("button_circle",   1, 0x04)
    k.step_bits("button_triangle", 1, 0x08)
    k.step_bits("button_start",  2, 0x02)
    k.step_bits("button_select", 2, 0x01)
    k.step_bits("button_ps",     2, 0x10)
    for key, hat in (("dpad_up", 0x00), ("dpad_down", 0x04),
                     ("dpad_left", 0x06), ("dpad_right", 0x02)):
        k.step_set_byte(key, 3, hat)
    k.write()


# Spec: PlasticBand 4-Lane Drums/Xbox 360.md (read via gh fallback below if
# present; reused FillXInputReport layout). Velocities packed into stick
# axis halves: LY=green/red, RX=yellow/blue, RY=orange/kick.
def gen_x360_rb_drums() -> None:
    rep_len = 17
    idle = [0] * rep_len
    idle[3] = idle[4] = 0x80
    idle[5] = idle[6] = 0x80
    k = Kit(
        "x360_rb_drums.raw.jsonl",
        {"vendor_id": "0x1bad", "product_id": "0x0003",
         "device_name": "Xbox 360 RB Drums (synthetic)",
         "device_type": "drum", "source": "xinput",
         "report_length": rep_len},
        idle,
    )
    # green = LY low byte (byte 11). red = LY high (byte 12). yellow = RX low
    # (byte 13). blue = RX high (byte 14). kick = byte 15 / 16 from RY.
    pads = [
        ("green_pad",    11, 0, 0x01),
        ("red_pad",      12, 0, 0x02),
        ("yellow_pad",   13, 0, 0x08),
        ("blue_pad",     14, 0, 0x04),
    ]
    for key, vbyte, _fbyte, fmask in pads:
        def make_mutator(vb, fm):
            def m(f, pressed):
                if pressed:
                    f[vb] = 0x7F
                    f[0] |= fm
            return m
        k._press_cycle(key, make_mutator(vbyte, fmask))
    k.step_bits("kick_pedal", 0, 0x40)  # right shoulder (XInput RB)
    k.step_bits("button_start",  1, 0x01)
    k.step_bits("button_select", 1, 0x02)
    for key, bit in (("dpad_up", 0), ("dpad_down", 1),
                     ("dpad_left", 2), ("dpad_right", 3)):
        k.step_bits(key, 2, 1 << bit)
    k.write()


# Spec: PlasticBand 5-Lane Drums/Xbox 360.md — adds orange pad in the 5th
# lane. RB-style XInput map plus orange on RB face button.
def gen_x360_gh_drums() -> None:
    rep_len = 17
    idle = [0] * rep_len
    idle[3] = idle[4] = 0x80
    idle[5] = idle[6] = 0x80
    k = Kit(
        "x360_gh_drums.raw.jsonl",
        {"vendor_id": "0x1bad", "product_id": "0x3110",
         "device_name": "Xbox 360 5-lane GH Drums (synthetic)",
         "device_type": "drum", "source": "xinput",
         "report_length": rep_len},
        idle,
    )
    pads = [
        ("green_pad",    11, 0, 0x01),
        ("red_pad",      12, 0, 0x02),
        ("yellow_pad",   13, 0, 0x08),
        ("blue_pad",     14, 0, 0x04),
        ("orange_pad",   15, 0, 0x20),
    ]
    for key, vbyte, _fbyte, fmask in pads:
        def make_mutator(vb, fm):
            def m(f, pressed):
                if pressed:
                    f[vb] = 0x7F
                    f[0] |= fm
            return m
        k._press_cycle(key, make_mutator(vbyte, fmask))
    k.step_bits("kick_pedal", 0, 0x40)
    k.step_bits("button_start",  1, 0x01)
    k.step_bits("button_select", 1, 0x02)
    for key, bit in (("dpad_up", 0), ("dpad_down", 1),
                     ("dpad_left", 2), ("dpad_right", 3)):
        k.step_bits(key, 2, 1 << bit)
    k.write()


def main() -> int:
    for g in (gen_ps3_rb_guitar, gen_ps4_rb_mustang, gen_ps5_riffmaster,
              gen_x360_rb_guitar, gen_x360_gh_guitar,
              gen_ps3_rb_drums, gen_ps4_rb_drums_pro,
              gen_x360_rb_drums, gen_x360_gh_drums):
        g()
    return 0


if __name__ == "__main__":
    sys.exit(main())
