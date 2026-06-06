// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "input/midi_input.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#include "common/logging/log.h"

#include <RtMidi.h>

// One backend for all three platforms: RtMidi compiles in ALSA on Linux,
// CoreMIDI on macOS, and WinMM on Windows. The public Input::MidiInput
// surface stays unchanged — the wizard and the runtime poll loop don't
// care which transport is below.

namespace Input::MidiInput {

namespace {

// General MIDI Drum Map (channel 10) defaults — used both to seed the
// snapshot writer and as the wizard's "expected note for this pad"
// hints. Real kits vary; the wizard's mapping uses the highest-velocity
// note observed during each probe step rather than this table.
struct PadDefault {
    int snap_byte;
    std::uint8_t mask_bit;
    std::initializer_list<int> notes;
};

const PadDefault kPadDefaults[] = {
    {kSnapByteKick,       0x10, {35, 36}},
    {kSnapByteSnareRed,   0x02, {38, 40}},
    {kSnapByteTomHighYel, 0x08, {48, 50}},
    {kSnapByteTomMidBlue, 0x04, {45, 47}},
    {kSnapByteTomLowGrn,  0x01, {41, 43}},
    {kSnapByteCymYellow,  0x08, {42, 44, 46, 51, 53}},
    {kSnapByteCymBlue,    0x04, {49, 57, 55, 52}},
    {kSnapByteCymGreen,   0x01, {49, 51, 57}},
};

constexpr auto kVelocityHoldMs = std::chrono::milliseconds(80);

struct PadState {
    std::uint8_t velocity = 0;
    std::chrono::steady_clock::time_point last_active{};
};

// One open MIDI port handle. Holds the RtMidiIn instance bound to a
// specific port plus the per-pad state machine and (optionally) a
// kit-specific note → byte override map.
struct PortHandle {
    std::unique_ptr<RtMidiIn> midi;
    std::chrono::steady_clock::time_point opened_at{};
    PadState pads[12]{};
    PadState pads_by_byte[16]{};  // kSnapshotBytes
    std::map<std::uint8_t, int> custom_map;
};

std::mutex g_global_mu;

// Build a friendly client name for RtMidi enumeration. RtMidi exposes
// its internal client to the OS; this name is what shows up in e.g.
// QjackCtl / `aplaymidi -l` so users can tell the emulator's MIDI
// subscriber apart from anything else they're running.
constexpr const char* kClientName = "shadPS4";

}  // namespace

bool EnsureInit() {
    // RtMidi creates its OS client on RtMidiIn construction; no global
    // init step needed. The function is still kept on the public API
    // so callers don't have to know the difference between transports.
    return true;
}

std::vector<PortInfo> EnumerateInputPorts() {
    std::vector<PortInfo> out;
    try {
        RtMidiIn probe(RtMidi::UNSPECIFIED, kClientName);
        const unsigned int n = probe.getPortCount();
        for (unsigned int i = 0; i < n; ++i) {
            PortInfo info;
            // Use the index as the id — RtMidi addresses ports by
            // numeric index, which can drift if the user plugs / unplugs
            // devices. For our use case (wizard captures + per-launch
            // runtime open) this is acceptable; users who hot-plug
            // during play would notice anyway because the kit would
            // disappear. A future revision could embed the port name
            // into the id for a more stable handle.
            info.id = std::to_string(i);
            info.name = probe.getPortName(i);
            out.push_back(std::move(info));
        }
    } catch (const RtMidiError& e) {
        LOG_WARNING(Input, "MIDI: enumeration failed: {}", e.getMessage());
    }
    return out;
}

void* OpenInputPort(const std::string& id) {
    int idx = -1;
    try { idx = std::stoi(id); } catch (...) { idx = -1; }
    if (idx < 0) {
        LOG_WARNING(Input, "MIDI: malformed port id {}", id);
        return nullptr;
    }
    try {
        auto handle = std::make_unique<PortHandle>();
        handle->midi = std::make_unique<RtMidiIn>(RtMidi::UNSPECIFIED, kClientName);
        if (idx >= (int)handle->midi->getPortCount()) {
            LOG_WARNING(Input, "MIDI: port index {} out of range ({} available)",
                        idx, handle->midi->getPortCount());
            return nullptr;
        }
        handle->midi->openPort(static_cast<unsigned int>(idx),
                                "shadPS4 MIDI in");
        // Ignore SysEx, timing, and active-sensing — we only care about
        // Note On / Off.
        handle->midi->ignoreTypes(true, true, true);
        handle->opened_at = std::chrono::steady_clock::now();
        LOG_INFO(Input, "MIDI: opened port {} ({})",
                 idx, handle->midi->getPortName(idx));
        return handle.release();
    } catch (const RtMidiError& e) {
        LOG_WARNING(Input, "MIDI: open failed for port {}: {}",
                    id, e.getMessage());
        return nullptr;
    }
}

void CloseInputPort(void* handle) {
    if (!handle) return;
    auto* h = static_cast<PortHandle*>(handle);
    try {
        if (h->midi && h->midi->isPortOpen()) h->midi->closePort();
    } catch (...) {
    }
    delete h;
}

namespace {

// Drain RtMidi's queue of pending messages into both the caller's event
// vector AND the per-pad / per-byte state arrays on the handle. RtMidi's
// getMessage returns one message at a time and returns the delta time
// since the previous message, in seconds — we ignore that and use
// our own wall-clock for the t_ms field, which keeps the API consistent
// with the wizard's per-step convention.
std::vector<NoteEvent> DrainAndUpdate(PortHandle* h) {
    std::vector<NoteEvent> out;
    if (!h->midi || !h->midi->isPortOpen()) return out;
    const auto now = std::chrono::steady_clock::now();
    std::vector<unsigned char> msg;
    while (true) {
        try {
            (void)h->midi->getMessage(&msg);
        } catch (const RtMidiError& e) {
            LOG_WARNING(Input, "MIDI: getMessage failed: {}", e.getMessage());
            break;
        }
        if (msg.empty()) break;
        // Channel messages are 2 or 3 bytes. We care about 0x80 (Note
        // Off) and 0x90 (Note On) on any channel; lower nibble is the
        // channel number, ignored.
        const unsigned char status = msg[0] & 0xF0;
        bool note_on = false;
        bool note_off = false;
        int note = 0;
        int vel = 0;
        if (status == 0x90 && msg.size() >= 3) {
            note = msg[1] & 0x7F;
            vel = msg[2] & 0x7F;
            if (vel > 0) note_on = true; else note_off = true;
        } else if (status == 0x80 && msg.size() >= 3) {
            note = msg[1] & 0x7F;
            vel = msg[2] & 0x7F;
            note_off = true;
        } else {
            continue;
        }

        NoteEvent record;
        record.on = note_on;
        record.note = static_cast<std::uint8_t>(note);
        record.velocity = static_cast<std::uint8_t>(vel);
        record.t_ms = static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - h->opened_at).count());
        out.push_back(record);

        if (!h->custom_map.empty()) {
            auto it = h->custom_map.find(record.note);
            if (it != h->custom_map.end()) {
                const int byte_idx = it->second;
                if (byte_idx >= 0 && byte_idx < (int)kSnapshotBytes) {
                    if (note_on) {
                        h->pads_by_byte[byte_idx].velocity =
                            static_cast<std::uint8_t>((vel * 255 + 63) / 127);
                        h->pads_by_byte[byte_idx].last_active = now;
                    } else {
                        h->pads_by_byte[byte_idx].velocity = 0;
                    }
                }
            }
        } else {
            for (int p = 0; p < (int)(sizeof(kPadDefaults) / sizeof(kPadDefaults[0])); ++p) {
                bool match = false;
                for (int n : kPadDefaults[p].notes) {
                    if (n == note) { match = true; break; }
                }
                if (!match) continue;
                if (note_on) {
                    h->pads[p].velocity =
                        static_cast<std::uint8_t>((vel * 255 + 63) / 127);
                    h->pads[p].last_active = now;
                } else {
                    h->pads[p].velocity = 0;
                }
                break;
            }
        }
    }
    // Decay pads whose last activity is older than the hold window.
    for (auto& p : h->pads) {
        if (p.velocity == 0) continue;
        if (now - p.last_active > kVelocityHoldMs) p.velocity = 0;
    }
    for (auto& p : h->pads_by_byte) {
        if (p.velocity == 0) continue;
        if (now - p.last_active > kVelocityHoldMs) p.velocity = 0;
    }
    return out;
}

}  // namespace

std::vector<NoteEvent> DrainEvents(void* handle) {
    if (!handle) return {};
    auto* h = static_cast<PortHandle*>(handle);
    std::lock_guard lk(g_global_mu);
    return DrainAndUpdate(h);
}

void ConfigurePadMap(void* handle, const std::map<std::uint8_t, int>& note_to_byte) {
    if (!handle) return;
    auto* h = static_cast<PortHandle*>(handle);
    std::lock_guard lk(g_global_mu);
    h->custom_map = note_to_byte;
    for (auto& p : h->pads) p.velocity = 0;
    for (auto& p : h->pads_by_byte) p.velocity = 0;
}

std::size_t SnapshotDrumBuffer(void* handle, std::uint8_t* out,
                               std::size_t out_len) {
    if (!handle || !out || out_len < kSnapshotBytes) return 0;
    auto* h = static_cast<PortHandle*>(handle);
    {
        std::lock_guard lk(g_global_mu);
        (void)DrainAndUpdate(h);
    }
    std::memset(out, 0, kSnapshotBytes);
    std::uint8_t flags = 0;
    if (!h->custom_map.empty()) {
        for (int b = 0; b < (int)kSnapshotBytes; ++b) {
            if (h->pads_by_byte[b].velocity == 0) continue;
            out[b] = h->pads_by_byte[b].velocity;
            flags |= static_cast<std::uint8_t>(1u << (b & 7));
        }
    } else {
        for (int p = 0; p < (int)(sizeof(kPadDefaults) / sizeof(kPadDefaults[0])); ++p) {
            const auto& pad = h->pads[p];
            if (pad.velocity == 0) continue;
            out[kPadDefaults[p].snap_byte] = pad.velocity;
            flags |= kPadDefaults[p].mask_bit;
        }
    }
    out[kSnapByteFaceFlags] = flags;
    return kSnapshotBytes;
}

void Shutdown() {
    // Per-port handles are owned by callers; nothing global to release —
    // RtMidiIn destructors close their OS clients automatically.
}

}  // namespace Input::MidiInput
