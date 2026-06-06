// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// MIDI capture data types and DeriveMidiKitToml. Parallel to
// hid_kit_probe_data.h but for native MIDI events instead of byte-grid
// snapshots — the user picked option (a) for capture format, so each
// step stores the raw Note On / Note Off events the device sent, with
// timestamps. The derive then walks per-step velocity stats and emits a
// drum-kit TOML mapping each step (red_pad, kick_pedal, ...) to the
// MIDI note(s) most associated with that step.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Input::MidiInstrument {

struct MidiEvent {
    bool on = false;             // Note On (velocity > 0) or Note Off
    std::uint8_t note = 0;       // 0..127
    std::uint8_t velocity = 0;   // 0..127, as the device sent it
    std::uint32_t t_ms = 0;      // ms relative to step start
};

struct MidiStepResult {
    std::string key;     // "_idle_baseline", "red_pad", etc.
    bool captured = false;
    std::vector<MidiEvent> events;
};

enum class MidiDeviceType {
    Drum,     // 4-lane (red/blue/yellow/green + kick), no cymbal split
    ProDrum,  // pads + cymbals + 2nd kick optional
};

struct MidiKitProbeData {
    // Meta. The wire format for these mirrors HID's meta record so a
    // future viewer can show all captures with one parser.
    int version = 1;
    std::string device_name;     // device + port pretty name
    std::string port_id;          // OS-specific port handle ("client:port" on ALSA)
    std::string source = "midi";
    MidiDeviceType device_type = MidiDeviceType::Drum;
    std::string timestamp;        // ISO-8601 capture time
    std::string capture_uuid;     // random UUID per capture
    std::string host_hash;        // SHA-256(salt + machineUniqueId), first 16 hex
    std::vector<MidiStepResult> results;
};

// Walks the per-step event lists, identifies which MIDI note(s) belong
// to each pad, and emits a TOML kit definition the runtime loads via
// LoadMidiKitFromToml (forthcoming runtime plumbing). Output schema is
// "shadps4-midi-instrument/v1" — distinct from the HID schema so the
// loader can route by header without sniffing fields.
std::string DeriveMidiKitToml(const MidiKitProbeData& data);

}  // namespace Input::MidiInstrument
