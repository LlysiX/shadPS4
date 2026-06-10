// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Cross-platform MIDI input backend for the RB4-instruments work — used
// to drive electronic drum kits (Roland TD-series, Alesis Nitro, Yamaha
// DTX, etc.) into the same legacy-instrument pipeline the HID/XInput
// guitars and drums go through.
//
// Architecture: MIDI is event-driven (Note On / Note Off / Control Change
// with timestamps), but everything downstream of GetLatestReport expects
// a periodic byte buffer the runtime polls every game tick. The bridge:
// MidiInput maintains a per-pad velocity map, decayed over a short hold
// after Note Off (or after a configurable timeout if the device doesn't
// send Note Off, which some kits don't), and snapshots that state into a
// fixed-layout byte buffer on demand. The buffer mirrors the PS4 RB
// 4-Lane drum wire format so the existing drum_ps4_layout packer code
// path applies unchanged.
//
// Today: Linux ALSA seq client is implemented. macOS and Windows
// EnumerateInputPorts return empty; calling OpenInputPort there returns
// nullptr. Proper Mac (CoreMIDI) and Win (WinMM) backends to follow.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Input::MidiInput {

// One enumerated MIDI input port. `id` is an opaque string the caller
// must pass back into OpenInputPort — it round-trips through TOML in
// the per-player device assignment, so it must be stable across boots
// (an ALSA "client:port" pair, a CoreMIDI endpoint UID hex, etc.).
struct PortInfo {
    std::string id;
    std::string name;
};

// Lazy init the underlying MIDI subsystem on first call. Safe to call
// from any thread; subsequent calls are cheap. Returns false if the
// host has no MIDI support compiled in (Win/Mac stubs today) — callers
// then treat EnumerateInputPorts as empty.
bool EnsureInit();

std::vector<PortInfo> EnumerateInputPorts();

// Open the given port for read. Returns an opaque handle, or nullptr
// on failure. The handle owns the resource; CloseInputPort must be
// called for every successful open.
void* OpenInputPort(const std::string& id);
void CloseInputPort(void* handle);

// One MIDI event drained from the port. Timestamps are in ms relative to
// the port's open time, useful for replaying captures at the right speed
// and for detecting modules that never send Note Off.
struct NoteEvent {
    bool on;               // true = Note On (with velocity > 0); false = Note Off
    std::uint8_t note;     // MIDI note number 0..127
    std::uint8_t velocity; // 0..127 (7-bit, as the device sent it)
    std::uint32_t t_ms;    // milliseconds since OpenInputPort returned
};

// Drain every MIDI event the kernel has queued for `handle` since the
// last DrainEvents call. The wizard uses this directly to write raw
// event records into the capture .jsonl (option-a format); the runtime
// gets the same events via SnapshotDrumBuffer below, which calls
// DrainEvents internally before reading state.
std::vector<NoteEvent> DrainEvents(void* handle);

// Install a per-kit MIDI-note → snapshot-byte-index map on `handle`.
// Used by the runtime to honour the wizard-derived [midi_pad_map] for a
// particular kit instead of the hardcoded General-MIDI defaults. Map
// keys are 0..127 (MIDI note number); values are byte indices into the
// snapshot buffer (the kSnapByte* constants). Passing an empty map
// clears the override and restores defaults — useful when switching
// between modules without closing the port.
void ConfigurePadMap(void* handle, const std::map<std::uint8_t, int>& note_to_byte);

// Snapshot the current per-pad velocity state into `out` (size N).
// Layout mirrors the PS4 RB 4-Lane Drum wire format — see the kSnapshot*
// constants below for the byte index of each pad. Velocities decay from
// the value reported at the last Note On to 0 over ~80 ms unless a Note
// Off arrives first, which clamps immediately. Pads not currently active
// read 0. Returns the number of bytes written (always kSnapshotBytes
// for a non-null handle).
constexpr std::size_t kSnapshotBytes = 16;

// Byte indices the snapshot writes into. The wizard's DeriveKitToml
// picks each drum_*_byte by finding the byte with maximum range during
// the corresponding probe step, so this layout is implementation
// detail — callers that care use the offsets here directly.
constexpr int kSnapByteFaceFlags = 0; // pad-press bitmap (bit per slot)
constexpr int kSnapByteKick = 1;
constexpr int kSnapByteSnareRed = 3;
constexpr int kSnapByteTomHighYel = 4;
constexpr int kSnapByteTomMidBlue = 5;
constexpr int kSnapByteTomLowGrn = 6;
constexpr int kSnapByteCymYellow = 8;
constexpr int kSnapByteCymBlue = 9;
constexpr int kSnapByteCymGreen = 10;

std::size_t SnapshotDrumBuffer(void* handle, std::uint8_t* out, std::size_t out_len);

// Shutdown clean-up — closes any still-open handles, exits the ALSA
// client. Called on emulator exit.
void Shutdown();

namespace Testing {

// Allocate a PortHandle without going through RtMidi — used by hidtest
// to exercise the DrainAndUpdate / SnapshotDrumBuffer state machine
// without needing a real MIDI device on the test host. Free with
// DestroySyntheticHandleForTesting; standard CloseInputPort would also
// work but does an unnecessary RtMidi cancelCallback dance on a handle
// that never set one up.
void* CreateSyntheticHandleForTesting();
void DestroySyntheticHandleForTesting(void* handle);

// Inject a note-on/off directly into the handle's pending queue,
// bypassing the RtMidi callback. The pushed event behaves exactly like
// one delivered live (subject to the same DrainAndUpdate / consume-on-
// read pipeline). Used to test the fast-play hit-detection invariant
// without a physical MIDI source.
void PushNoteEventForTesting(void* handle, std::uint8_t note, std::uint8_t velocity, bool note_on);

} // namespace Testing

} // namespace Input::MidiInput
