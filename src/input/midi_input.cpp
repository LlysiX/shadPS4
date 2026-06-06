// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "input/midi_input.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#include "common/logging/log.h"

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
    {kSnapByteKick,       0x10, {35, 36}},                  // kick / bass drum
    {kSnapByteSnareRed,   0x02, {38, 40}},                  // snare 1/2
    {kSnapByteTomHighYel, 0x08, {48, 50}},                  // hi tom 1/2
    {kSnapByteTomMidBlue, 0x04, {45, 47}},                  // mid tom 1/2
    {kSnapByteTomLowGrn,  0x01, {41, 43}},                  // floor tom 1/2
    {kSnapByteCymYellow,  0x08, {42, 44, 46, 51, 53}},      // hi-hat (closed/open/pedal), ride bell, ride
    {kSnapByteCymBlue,    0x04, {49, 57, 55, 52}},          // crash 1/2, splash, chinese
    {kSnapByteCymGreen,   0x01, {49, 51, 57}},              // overlap with cym1/yellow — wizard disambiguates
};

constexpr auto kVelocityHoldMs = std::chrono::milliseconds(80);

// Per-pad live state held inside an open MIDI port handle. velocity is
// the most recent Note On's value (8-bit, expanded from the 7-bit MIDI
// value at snapshot time so the existing drum_*_byte velocity packer
// applies the same scaling logic as for HID kits). last_active is the
// time of the most recent Note On; once now - last_active exceeds
// kVelocityHoldMs, the pad's velocity falls back to 0 even without a
// Note Off — Roland TDs and most kits send Note Off promptly, but a
// handful of cheaper modules don't.
struct PadState {
    std::uint8_t velocity = 0;
    std::chrono::steady_clock::time_point last_active{};
};

}  // namespace

// =============================================================================
// Linux ALSA-seq backend
// =============================================================================
#if defined(__linux__)
#include <alsa/asoundlib.h>

namespace {

struct LinuxState {
    snd_seq_t* seq = nullptr;
    int client_id = -1;
    int local_port = -1;
};

std::mutex g_state_mu;
LinuxState g_state;

bool EnsureInitInternal() {
    std::lock_guard lk(g_state_mu);
    if (g_state.seq) return true;
    snd_seq_t* seq = nullptr;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) {
        LOG_WARNING(Input, "MIDI: snd_seq_open failed");
        return false;
    }
    snd_seq_set_client_name(seq, "shadPS4");
    const int port = snd_seq_create_simple_port(
        seq, "shadPS4 MIDI in",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_APPLICATION);
    if (port < 0) {
        LOG_WARNING(Input, "MIDI: snd_seq_create_simple_port failed");
        snd_seq_close(seq);
        return false;
    }
    g_state.seq = seq;
    g_state.client_id = snd_seq_client_id(seq);
    g_state.local_port = port;
    LOG_INFO(Input, "MIDI: ALSA seq client={} local_port={}",
             g_state.client_id, g_state.local_port);
    return true;
}

struct LinuxPortHandle {
    int subscribed_client = -1;
    int subscribed_port = -1;
    PadState pads[12]{};  // indexed by kPadDefaults order
    std::chrono::steady_clock::time_point opened_at{};
};

}  // namespace

bool EnsureInit() {
    return EnsureInitInternal();
}

std::vector<PortInfo> EnumerateInputPorts() {
    std::vector<PortInfo> out;
    if (!EnsureInitInternal()) return out;
    std::lock_guard lk(g_state_mu);
    snd_seq_t* seq = g_state.seq;
    snd_seq_client_info_t* cinfo;
    snd_seq_port_info_t* pinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);
    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(seq, cinfo) >= 0) {
        const int client = snd_seq_client_info_get_client(cinfo);
        if (client == g_state.client_id) continue;  // skip our own
        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(seq, pinfo) >= 0) {
            const unsigned cap = snd_seq_port_info_get_capability(pinfo);
            constexpr unsigned needed =
                SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
            if ((cap & needed) != needed) continue;
            const int port = snd_seq_port_info_get_port(pinfo);
            PortInfo info;
            info.id = std::to_string(client) + ":" + std::to_string(port);
            const char* client_name = snd_seq_client_info_get_name(cinfo);
            const char* port_name = snd_seq_port_info_get_name(pinfo);
            info.name = std::string(client_name ? client_name : "?") + " — " +
                        std::string(port_name ? port_name : "?");
            out.push_back(std::move(info));
        }
    }
    return out;
}

void* OpenInputPort(const std::string& id) {
    if (!EnsureInitInternal()) return nullptr;
    int client = 0, port = 0;
    if (std::sscanf(id.c_str(), "%d:%d", &client, &port) != 2) {
        LOG_WARNING(Input, "MIDI: malformed port id {}", id);
        return nullptr;
    }
    std::lock_guard lk(g_state_mu);
    snd_seq_addr_t sender{(unsigned char)client, (unsigned char)port};
    snd_seq_addr_t dest{(unsigned char)g_state.client_id,
                        (unsigned char)g_state.local_port};
    snd_seq_port_subscribe_t* sub;
    snd_seq_port_subscribe_alloca(&sub);
    snd_seq_port_subscribe_set_sender(sub, &sender);
    snd_seq_port_subscribe_set_dest(sub, &dest);
    if (snd_seq_subscribe_port(g_state.seq, sub) < 0) {
        LOG_WARNING(Input, "MIDI: subscribe to {}:{} failed", client, port);
        return nullptr;
    }
    auto* h = new LinuxPortHandle();
    h->subscribed_client = client;
    h->subscribed_port = port;
    h->opened_at = std::chrono::steady_clock::now();
    LOG_INFO(Input, "MIDI: opened port {}:{}", client, port);
    return h;
}

void CloseInputPort(void* handle) {
    if (!handle) return;
    auto* h = static_cast<LinuxPortHandle*>(handle);
    std::lock_guard lk(g_state_mu);
    if (g_state.seq && h->subscribed_client >= 0) {
        snd_seq_addr_t sender{(unsigned char)h->subscribed_client,
                              (unsigned char)h->subscribed_port};
        snd_seq_addr_t dest{(unsigned char)g_state.client_id,
                            (unsigned char)g_state.local_port};
        snd_seq_port_subscribe_t* sub;
        snd_seq_port_subscribe_alloca(&sub);
        snd_seq_port_subscribe_set_sender(sub, &sender);
        snd_seq_port_subscribe_set_dest(sub, &dest);
        snd_seq_unsubscribe_port(g_state.seq, sub);
    }
    delete h;
}

namespace {

// Internal: drain ALSA seq events into a vector for the caller, AND
// also update the handle's per-pad state machine + apply velocity
// decay. Both the public DrainEvents (wizard capture path) and
// SnapshotDrumBuffer (runtime path) funnel through here so the
// underlying ALSA event queue is only consumed once per call.
std::vector<NoteEvent> DrainAndUpdate(LinuxPortHandle* h) {
    std::vector<NoteEvent> out;
    if (!g_state.seq) return out;
    const auto now = std::chrono::steady_clock::now();
    snd_seq_event_t* ev = nullptr;
    while (snd_seq_event_input(g_state.seq, &ev) >= 0 && ev) {
        const int note = ev->data.note.note;
        const int vel = ev->data.note.velocity;
        bool note_on = false;
        bool note_off = false;
        switch (ev->type) {
        case SND_SEQ_EVENT_NOTEON:
            if (vel > 0) note_on = true; else note_off = true;
            break;
        case SND_SEQ_EVENT_NOTEOFF:
            note_off = true;
            break;
        default:
            // ignore controllers / pitchbend / aftertouch / sysex etc.
            snd_seq_free_event(ev);
            continue;
        }
        // Append to the caller-visible event stream.
        NoteEvent record;
        record.on = note_on;
        record.note = static_cast<std::uint8_t>(note & 0x7F);
        record.velocity = static_cast<std::uint8_t>(vel & 0x7F);
        record.t_ms = static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - h->opened_at)
                .count());
        out.push_back(record);
        // Update the state machine too (used by SnapshotDrumBuffer).
        for (int p = 0; p < (int)(sizeof(kPadDefaults) / sizeof(kPadDefaults[0])); ++p) {
            bool match = false;
            for (int n : kPadDefaults[p].notes) {
                if (n == note) { match = true; break; }
            }
            if (!match) continue;
            if (note_on) {
                h->pads[p].velocity =
                    static_cast<std::uint8_t>((vel * 255 + 63) / 127);  // 0..127 -> 0..255
                h->pads[p].last_active = now;
            } else {
                h->pads[p].velocity = 0;
            }
            break;
        }
        snd_seq_free_event(ev);
    }
    // Decay any pads whose Note On is older than the hold window — covers
    // modules that never send Note Off (or send it on a different chan).
    for (auto& p : h->pads) {
        if (p.velocity == 0) continue;
        if (now - p.last_active > kVelocityHoldMs) p.velocity = 0;
    }
    return out;
}

}  // namespace

std::vector<NoteEvent> DrainEvents(void* handle) {
    if (!handle) return {};
    auto* h = static_cast<LinuxPortHandle*>(handle);
    std::lock_guard lk(g_state_mu);
    return DrainAndUpdate(h);
}

std::size_t SnapshotDrumBuffer(void* handle, std::uint8_t* out,
                               std::size_t out_len) {
    if (!handle || !out || out_len < kSnapshotBytes) return 0;
    auto* h = static_cast<LinuxPortHandle*>(handle);
    {
        std::lock_guard lk(g_state_mu);
        (void)DrainAndUpdate(h);  // runtime path: events drained into state, return value ignored
    }
    std::memset(out, 0, kSnapshotBytes);
    std::uint8_t flags = 0;
    for (int p = 0; p < (int)(sizeof(kPadDefaults) / sizeof(kPadDefaults[0])); ++p) {
        const auto& pad = h->pads[p];
        if (pad.velocity == 0) continue;
        out[kPadDefaults[p].snap_byte] = pad.velocity;
        flags |= kPadDefaults[p].mask_bit;
    }
    out[kSnapByteFaceFlags] = flags;
    return kSnapshotBytes;
}

void Shutdown() {
    std::lock_guard lk(g_state_mu);
    if (g_state.seq) {
        snd_seq_close(g_state.seq);
        g_state.seq = nullptr;
        g_state.client_id = -1;
        g_state.local_port = -1;
    }
}

// =============================================================================
// Stubs for macOS / Windows — proper backends to follow.
// =============================================================================
#else
bool EnsureInit() { return false; }
std::vector<PortInfo> EnumerateInputPorts() { return {}; }
void* OpenInputPort(const std::string&) { return nullptr; }
void CloseInputPort(void*) {}
std::vector<NoteEvent> DrainEvents(void*) { return {}; }
std::size_t SnapshotDrumBuffer(void*, std::uint8_t*, std::size_t) { return 0; }
void Shutdown() {}
#endif

}  // namespace Input::MidiInput
