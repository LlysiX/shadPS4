// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "input/midi_input.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
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
    {kSnapByteKick, 0x10, {35, 36}},
    {kSnapByteSnareRed, 0x02, {38, 40}},
    {kSnapByteTomHighYel, 0x08, {48, 50}},
    {kSnapByteTomMidBlue, 0x04, {45, 47}},
    {kSnapByteTomLowGrn, 0x01, {41, 43}},
    {kSnapByteCymYellow, 0x08, {42, 44, 46, 51, 53}},
    {kSnapByteCymBlue, 0x04, {49, 57, 55, 52}},
    {kSnapByteCymGreen, 0x01, {49, 51, 57}},
};

constexpr auto kVelocityHoldMs = std::chrono::milliseconds(80);

struct PadState {
    std::uint8_t velocity = 0;
    std::chrono::steady_clock::time_point last_active{};
};

// Cap pending callback events to bound memory if a consumer stops
// draining. 4096 note-on/offs is ~20 seconds of fast continuous
// double-pedal drumming — well past any real burst.
constexpr std::size_t kMaxPendingEvents = 4096;

// One open MIDI port handle. Holds the RtMidiIn instance bound to a
// specific port plus the per-pad state machine and (optionally) a
// kit-specific note → byte override map.
struct PortHandle {
    std::unique_ptr<RtMidiIn> midi;
    std::chrono::steady_clock::time_point opened_at{};
    PadState pads[12]{};
    PadState pads_by_byte[16]{}; // kSnapshotBytes
    std::map<std::uint8_t, int> custom_map;
    // Filtered note-on/off events queued by the RtMidi callback for the
    // next DrainAndUpdate. We attach a callback in OpenInputPort so
    // RtMidi's internal message queue is bypassed entirely — Pro Drum
    // modules stream CC4 (hi-hat pedal position) and polyphonic
    // aftertouch continuously, and the default 1024-message queue would
    // fill in seconds of idle time and silently drop subsequent note-ons.
    std::deque<NoteEvent> pending;
};

std::mutex g_global_mu;

// Build a friendly client name for RtMidi enumeration. RtMidi exposes
// its internal client to the OS; this name is what shows up in e.g.
// QjackCtl / `aplaymidi -l` so users can tell the emulator's MIDI
// subscriber apart from anything else they're running.
constexpr const char* kClientName = "shadPS4";

} // namespace

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

namespace {

// RtMidi delivers every incoming MIDI message here, on its own dispatch
// thread. We filter to note-on / note-off at receive time and enqueue
// only those — CC (hi-hat pedal), polyphonic / channel aftertouch, and
// pitch-bend never take a slot in any queue. With this callback set
// RtMidi bypasses its internal 1024-message queue entirely, so a Pro
// Drum module idling on the hi-hat pedal can no longer flood the queue
// and drop subsequent note-ons.
void RtMidiCb(double /*deltatime*/, std::vector<unsigned char>* message, void* userData) {
    if (!message || message->empty() || !userData)
        return;
    const unsigned char status = (*message)[0] & 0xF0;
    if (status != 0x80 && status != 0x90)
        return;
    if (message->size() < 3)
        return;
    auto* h = static_cast<PortHandle*>(userData);
    NoteEvent rec;
    rec.note = static_cast<std::uint8_t>((*message)[1] & 0x7F);
    rec.velocity = static_cast<std::uint8_t>((*message)[2] & 0x7F);
    rec.on = (status == 0x90 && rec.velocity > 0);
    // Diagnostic: log every note-on we receive from the live MIDI port.
    // Drop to LOG_DEBUG once we've pinned down whether the user's Pro Drum module is sending notes that match the kit's midi_pad_map at gameplay time. If the in-game module mode differs from the probe mode (different MIDI program / preset), notes hit here but the
    // is sending notes that match the kit's midi_pad_map at
    // gameplay time. If the in-game module mode differs from the probe
    // mode (different MIDI program / preset), notes hit here but the
    // pads_by_byte writer in DrainAndUpdate finds no map entry and
    // SnapshotDrumBuffer returns all zeros — RB4 sees no drum hit
    // even though the runtime is functioning correctly. This log line
    // makes that diagnosable from a tap-each-pad-once test.
    if (rec.on) {
        LOG_INFO(Input, "MIDI rx: note_on note={} vel={} (status=0x{:x})", rec.note, rec.velocity,
                 (*message)[0]);
    }
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lk(g_global_mu);
    rec.t_ms = static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - h->opened_at).count());
    h->pending.push_back(rec);
    if (h->pending.size() > kMaxPendingEvents)
        h->pending.pop_front();
}

} // namespace

void* OpenInputPort(const std::string& id) {
    int idx = -1;
    try {
        idx = std::stoi(id);
    } catch (...) {
        idx = -1;
    }
    if (idx < 0) {
        LOG_WARNING(Input, "MIDI: malformed port id {}", id);
        return nullptr;
    }
    try {
        auto handle = std::make_unique<PortHandle>();
        // Third arg is queueSizeLimit — a generous safety net. With the
        // callback set below RtMidi bypasses the queue, but the limit
        // protects us if a future change ever removes the callback.
        handle->midi = std::make_unique<RtMidiIn>(RtMidi::UNSPECIFIED, kClientName, 16384);
        if (idx >= (int)handle->midi->getPortCount()) {
            LOG_WARNING(Input, "MIDI: port index {} out of range ({} available)", idx,
                        handle->midi->getPortCount());
            return nullptr;
        }
        // Filter SysEx / Time / Active-Sensing at the RtMidi level.
        handle->midi->ignoreTypes(true, true, true);
        handle->opened_at = std::chrono::steady_clock::now();
        // Register callback before openPort so the first inbound message
        // after the port is hot lands in our deque, not RtMidi's queue.
        handle->midi->setCallback(&RtMidiCb, handle.get());
        handle->midi->openPort(static_cast<unsigned int>(idx), "shadPS4 MIDI in");
        LOG_INFO(Input, "MIDI: opened port {} ({})", idx, handle->midi->getPortName(idx));
        return handle.release();
    } catch (const RtMidiError& e) {
        LOG_WARNING(Input, "MIDI: open failed for port {}: {}", id, e.getMessage());
        return nullptr;
    }
}

void CloseInputPort(void* handle) {
    if (!handle)
        return;
    auto* h = static_cast<PortHandle*>(handle);
    try {
        if (h->midi) {
            // Cancel the user-data callback before closePort so a tail
            // dispatch can't fire after we release g_global_mu and
            // delete h.
            h->midi->cancelCallback();
            if (h->midi->isPortOpen())
                h->midi->closePort();
        }
    } catch (...) {
    }
    std::lock_guard lk(g_global_mu);
    delete h;
}

namespace {

// Drain any note events the RtMidi callback queued since the last call
// into both the caller's event vector AND the per-pad / per-byte state
// arrays on the handle. Non-note messages were dropped at the callback
// boundary so the deque only ever holds NoteEvent records.
std::vector<NoteEvent> DrainAndUpdate(PortHandle* h) {
    std::vector<NoteEvent> out;
    if (!h->midi)
        return out;
    const auto now = std::chrono::steady_clock::now();
    while (!h->pending.empty()) {
        const NoteEvent record = h->pending.front();
        h->pending.pop_front();
        out.push_back(record);
        const int vel = record.velocity;
        const bool note_on = record.on;
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
                    if (n == record.note) {
                        match = true;
                        break;
                    }
                }
                if (!match)
                    continue;
                if (note_on) {
                    h->pads[p].velocity = static_cast<std::uint8_t>((vel * 255 + 63) / 127);
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
        if (p.velocity == 0)
            continue;
        if (now - p.last_active > kVelocityHoldMs)
            p.velocity = 0;
    }
    for (auto& p : h->pads_by_byte) {
        if (p.velocity == 0)
            continue;
        if (now - p.last_active > kVelocityHoldMs)
            p.velocity = 0;
    }
    return out;
}

} // namespace

std::vector<NoteEvent> DrainEvents(void* handle) {
    if (!handle)
        return {};
    auto* h = static_cast<PortHandle*>(handle);
    std::lock_guard lk(g_global_mu);
    return DrainAndUpdate(h);
}

void ConfigurePadMap(void* handle, const std::map<std::uint8_t, int>& note_to_byte) {
    if (!handle)
        return;
    auto* h = static_cast<PortHandle*>(handle);
    std::lock_guard lk(g_global_mu);
    h->custom_map = note_to_byte;
    for (auto& p : h->pads)
        p.velocity = 0;
    for (auto& p : h->pads_by_byte)
        p.velocity = 0;
}

std::size_t SnapshotDrumBuffer(void* handle, std::uint8_t* out, std::size_t out_len) {
    if (!handle || !out || out_len < kSnapshotBytes)
        return 0;
    auto* h = static_cast<PortHandle*>(handle);
    std::lock_guard lk(g_global_mu);
    (void)DrainAndUpdate(h);
    std::memset(out, 0, kSnapshotBytes);
    std::uint8_t flags = 0;
    if (!h->custom_map.empty()) {
        for (int b = 0; b < (int)kSnapshotBytes; ++b) {
            if (h->pads_by_byte[b].velocity == 0)
                continue;
            out[b] = h->pads_by_byte[b].velocity;
            std::uint8_t mask = 0;
            for (const auto& d : kPadDefaults) {
                if (d.snap_byte == b) {
                    mask = d.mask_bit;
                    break;
                }
            }
            flags |= mask;
        }
    } else {
        for (int p = 0; p < (int)(sizeof(kPadDefaults) / sizeof(kPadDefaults[0])); ++p) {
            const auto& pad = h->pads[p];
            if (pad.velocity == 0)
                continue;
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

} // namespace Input::MidiInput
