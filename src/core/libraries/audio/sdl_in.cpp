// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <common/config.h>
#include <common/logging/log.h>
#include "sdl_in.h"

namespace {
// RMS level of a block of signed-16-bit samples, in dBFS (0 dB = full
// scale). Returns a large negative number for digital silence.
float RmsDbS16(const int16_t* samples, int count) {
    if (count <= 0)
        return -120.0f;
    double sum_sq = 0.0;
    for (int i = 0; i < count; ++i) {
        const double s = static_cast<double>(samples[i]) / 32768.0;
        sum_sq += s * s;
    }
    const double rms = std::sqrt(sum_sq / static_cast<double>(count));
    if (rms <= 1e-7)
        return -120.0f;
    return static_cast<float>(20.0 * std::log10(rms));
}
// SDL picks per-OS defaults for the mic capture buffer — typically
// 1024–4096 frames, which is 23–93 ms at 44100 Hz. RB4 polls the mic on
// its vocal-mix path; if the first SDL chunk doesn't arrive until tens
// of ms after the game asked for it, the game's output buffer underruns
// and the *mic-derived* audio crackles. 256 frames (≈ 6 ms at 44100 Hz)
// gives SDL enough headroom while keeping first-chunk latency tight.
constexpr const char* kCaptureSampleFrames = "256";

// Per-call wait budget inside AudioInInput. Was 1000 ms with SDL_Delay(1)
// busy-polling — way too generous; any block longer than the host audio
// buffer is already a glitch. 100 ms keeps a stuck device from hanging
// the audio thread, but any single short hiccup returns partial-data
// quickly so the game can resync.
constexpr auto kInputTimeout = std::chrono::milliseconds(100);

// How long we'll wait between condvar wakeups before re-checking the
// available byte count. The put-callback wakes us as soon as data
// arrives, so this is just a safety net — short enough that we don't
// stall noticeably if the callback ever misfires.
constexpr auto kInputWaitSlice = std::chrono::milliseconds(5);
} // namespace

int SDLAudioIn::AudioInit() {
    return SDL_InitSubSystem(SDL_INIT_AUDIO);
}

void SDLCALL SDLAudioIn::OnStreamPut(void* userdata, SDL_AudioStream* /*stream*/,
                                     int /*additional_amount*/, int /*total_amount*/) {
    // SDL invokes this whenever the capture device pushes a new chunk
    // into the stream. We wake any AudioInInput call that's waiting for
    // more data on this port; the waiter re-checks SDL_GetAudioStreamAvailable
    // under the port mutex and drains what it needs.
    auto* port = static_cast<AudioInPort*>(userdata);
    if (!port || !port->data_cv)
        return;
    port->data_cv->notify_all();
}

int SDLAudioIn::AudioInOpen(int user_id, int type, uint32_t samples_num, uint32_t freq,
                            uint32_t format) {
    std::scoped_lock lock{m_mutex};

    for (int id = 0; id < static_cast<int>(portsIn.size()); ++id) {
        auto& port = portsIn[id];
        if (!port.isOpen) {
            port.isOpen = true;
            port.user_id = user_id;
            port.type = type;
            port.samples_num = samples_num;
            port.freq = freq;
            port.format = format;

            SDL_AudioFormat sampleFormat;
            switch (format) {
            case Libraries::AudioIn::ORBIS_AUDIO_IN_PARAM_FORMAT_S16_MONO:
                sampleFormat = SDL_AUDIO_S16;
                port.channels_num = 1;
                port.sample_size = 2;
                break;
            case Libraries::AudioIn::ORBIS_AUDIO_IN_PARAM_FORMAT_S16_STEREO:
                sampleFormat = SDL_AUDIO_S16;
                port.channels_num = 2;
                port.sample_size = 2;
                break;
            default:
                port.isOpen = false;
                return ORBIS_AUDIO_IN_ERROR_INVALID_PORT;
            }

            SDL_AudioSpec fmt;
            SDL_zero(fmt);
            fmt.format = sampleFormat;
            fmt.channels = port.channels_num;
            fmt.freq = port.freq;

            // Per-user device pick. user_id is 1..N in shadPS4 controller
            // convention; Config's slot index is 0..N-1. user_id outside
            // that range (e.g. SYSTEM 0xFF or INVALID -1) falls back to
            // slot 0 via the Config clamp.
            std::string micDevStr = Config::getMicDevice(user_id - 1);
            uint32_t devId = 0;

            bool nullDevice = false;
            if (micDevStr == "None") {
                nullDevice = true;
            } else if (micDevStr == "Default Device") {
                devId = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
            } else {
                // Player Assignment Overrides stores the device NAME
                // (SDL audio device IDs aren't stable across enumerations,
                // so storing the runtime handle would not survive a
                // dialog reopen). Look the name up in the current SDL
                // enumeration; fall back to the legacy numeric-id form
                // for users who saved via the older settings dialog.
                int count = 0;
                SDL_AudioDeviceID* devs = SDL_GetAudioRecordingDevices(&count);
                bool resolved = false;
                if (devs) {
                    for (int i = 0; i < count; ++i) {
                        const char* nm = SDL_GetAudioDeviceName(devs[i]);
                        if (nm && micDevStr == nm) {
                            devId = devs[i];
                            resolved = true;
                            break;
                        }
                    }
                    SDL_free(devs);
                }
                if (!resolved) {
                    try {
                        devId = static_cast<uint32_t>(std::stoul(micDevStr));
                        resolved = true;
                    } catch (const std::exception&) {
                        // fall through
                    }
                }
                if (!resolved) nullDevice = true;
            }

            // Request a small capture buffer so the very first chunk
            // arrives quickly. SDL reads this hint at device-open time;
            // setting it just before the SDL_OpenAudioDeviceStream call
            // is the smallest-scope way to apply it.
            SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, kCaptureSampleFrames);

            port.stream =
                nullDevice ? nullptr : SDL_OpenAudioDeviceStream(devId, &fmt, nullptr, nullptr);

            if (!port.stream) {
                // if stream is null, either due to configuration disabling the input,
                // or no input devices present in the system, still return a valid id
                // as some games require that (e.g. L.A. Noire)
                return id + 1;
            }

            // Wire the put-callback so SDL wakes our waiter when bytes
            // arrive, instead of us spinning on SDL_Delay(1). userdata
            // is the port pointer; portsIn is a fixed-size array member,
            // so its element addresses are stable for the SDLAudioIn
            // lifetime.
            if (!SDL_SetAudioStreamPutCallback(port.stream, OnStreamPut, &port)) {
                LOG_WARNING(Lib_AudioIn, "AudioInOpen: SDL_SetAudioStreamPutCallback failed: {}",
                            SDL_GetError());
                // Non-fatal — the wait loop falls back to its kInputWaitSlice
                // timeouts. The mic still works, just with the old poll
                // cadence.
            }

            if (SDL_ResumeAudioStreamDevice(port.stream) == false) {
                SDL_DestroyAudioStream(port.stream);
                port.Reset();
                return ORBIS_AUDIO_IN_ERROR_STREAM_FAIL;
            }

            return id + 1;
        }
    }

    return ORBIS_AUDIO_IN_ERROR_INVALID_PORT;
}

int SDLAudioIn::AudioInInput(int handle, void* out_buffer) {
    if (handle < 1 || handle > static_cast<int>(portsIn.size()) || !out_buffer) {
        return ORBIS_AUDIO_IN_ERROR_INVALID_PORT;
    }

    // Look up port state under the global mutex, then drop it for the
    // wait. Multiple ports must be able to drain concurrently; previously
    // m_mutex was held across SDL_Delay(1), so one mic blocked every
    // other mic.
    int sample_size = 0;
    int channels_num = 0;
    int samples_num = 0;
    AudioInPort* port_ptr = nullptr;
    bool stream_null = false;
    {
        std::scoped_lock lock{m_mutex};
        auto& port = portsIn[handle - 1];
        if (!port.isOpen)
            return ORBIS_AUDIO_IN_ERROR_INVALID_PORT;
        sample_size = port.sample_size;
        channels_num = port.channels_num;
        samples_num = port.samples_num;
        stream_null = (port.stream == nullptr);
        port_ptr = &port;
    }
    if (stream_null) {
        // Null-stream port (mic disabled). Match the old caller contract:
        // hand back zero-filled samples so games don't stall. Report
        // ACTIVE (not silent) — a game that gates its reads on
        // sceAudioInGetSilentState must keep calling AudioInInput; if we
        // said "silent" it could stop polling entirely.
        const int bytesToRead = samples_num * sample_size * channels_num;
        std::memset(out_buffer, 0, bytesToRead);
        if (port_ptr)
            port_ptr->silent.store(false, std::memory_order_relaxed);
        return samples_num;
    }

    const int bytesToRead = samples_num * sample_size * channels_num;
    const int frame_size = sample_size * channels_num;
    const auto deadline = std::chrono::steady_clock::now() + kInputTimeout;

    // Wait + drain happen while holding the per-port data mutex.
    // AudioInClose acquires the same mutex, clears port.stream to null,
    // then notifies; the waiter sees the null on its next iteration and
    // bails out before the SDL stream is destroyed. m_mutex isn't held
    // here, so open/close on OTHER ports keeps working normally.
    std::unique_lock data_lock{*port_ptr->data_mu};
    SDL_AudioStream* live = port_ptr->stream;
    if (!live)
        return ORBIS_AUDIO_IN_ERROR_INVALID_PORT;

    while (SDL_GetAudioStreamAvailable(live) < bytesToRead) {
        if (std::chrono::steady_clock::now() >= deadline)
            break;
        // wait_for atomically releases data_lock while sleeping; the
        // put callback notifies us as soon as SDL pushes more bytes
        // into the stream.
        port_ptr->data_cv->wait_for(data_lock, kInputWaitSlice);
        live = port_ptr->stream;
        if (!live)
            return ORBIS_AUDIO_IN_ERROR_INVALID_PORT;
    }

    const int avail = SDL_GetAudioStreamAvailable(live);
    if (avail <= 0) {
        // No bytes arrived within the timeout. Old behaviour returned
        // ORBIS_AUDIO_IN_ERROR_TIMEOUT here; keep that so games that
        // explicitly check for the error code (none known, but the ABI
        // documents it) don't break.
        return ORBIS_AUDIO_IN_ERROR_TIMEOUT;
    }

    // Honour partial reads — return however many full frames have
    // arrived so far, capped at the guest's request. PS4 sceAudioInInput
    // returns frames-actually-delivered, so partial is legal and lets
    // games self-clock against the real mic cadence instead of waiting
    // for a quantum that may not come.
    const int read_target = std::min(avail, bytesToRead);
    const int aligned_target = (read_target / frame_size) * frame_size;
    if (aligned_target <= 0)
        return ORBIS_AUDIO_IN_ERROR_TIMEOUT;

    const int bytesRead = SDL_GetAudioStreamData(live, out_buffer, aligned_target);
    if (bytesRead < 0) {
        LOG_ERROR(Lib_AudioIn, "AudioInInput error: {}", SDL_GetError());
        return ORBIS_AUDIO_IN_ERROR_STREAM_FAIL;
    }

    // Software noise gate. Still holding data_lock, so gate_open /
    // last_active are safe to touch; `silent` is atomic for the
    // cross-thread sceAudioInGetSilentState query. Enable + threshold
    // are per-user-slot so harmony singers can tune their own gate;
    // hold is shared because the tail-decay feel rarely varies per mic.
    const int slot = port_ptr->user_id - 1;  // Config clamps OOB internally
    // Update the VU peak unconditionally for any S16 chunk so the
    // Player Assignment dialog's meter reflects mic activity even
    // when the gate is disabled.
    float level_db = -120.0f;
    if (sample_size == 2 && bytesRead > 0) {
        const auto* samples = static_cast<const int16_t*>(out_buffer);
        const int sample_count = bytesRead / 2;
        level_db = RmsDbS16(samples, sample_count);
        port_ptr->peak_dbfs.store(level_db, std::memory_order_relaxed);
    }
    if (Config::getMicGateEnabled(slot) && sample_size == 2 && bytesRead > 0) {
        const float threshold_db = static_cast<float>(Config::getMicGateThresholdDb(slot));
        const auto hold = std::chrono::milliseconds(Config::getMicGateHoldMs());
        const auto now = std::chrono::steady_clock::now();

        if (level_db >= threshold_db) {
            port_ptr->gate_open = true;
            port_ptr->last_active = now;
        } else if (port_ptr->gate_open && (now - port_ptr->last_active) >= hold) {
            // Held quiet long enough — close the gate.
            port_ptr->gate_open = false;
        }

        if (!port_ptr->gate_open) {
            std::memset(out_buffer, 0, static_cast<std::size_t>(bytesRead));
            port_ptr->silent.store(true, std::memory_order_relaxed);
        } else {
            port_ptr->silent.store(false, std::memory_order_relaxed);
        }
    } else {
        // Gate disabled (or non-S16 format we don't analyse): always
        // report active so the game mixes the mic unchanged.
        port_ptr->gate_open = true;
        port_ptr->silent.store(false, std::memory_order_relaxed);
    }

    return bytesRead / frame_size;
}

float SDLAudioIn::GetPeakDbfs(int slot) {
    // user_id is 1..4 in the controller-slot convention. Walk open
    // ports to find the one bound to the requested slot. We don't index
    // by handle here because the dialog cares about "Player N's mic"
    // not "the Nth handle the game asked for".
    std::scoped_lock lock{m_mutex};
    for (auto& port : portsIn) {
        if (port.isOpen && port.user_id == slot + 1)
            return port.peak_dbfs.load(std::memory_order_relaxed);
    }
    return -120.0f;
}

bool SDLAudioIn::IsSilent(int handle) {
    // Per-slot gate state: when the gate is off for THIS player's slot,
    // the mic is never reported silent — matches the original always-
    // active stub and guarantees a game that gates its reads on
    // sceAudioInGetSilentState keeps polling the mic.
    std::scoped_lock lock{m_mutex};
    if (handle < 1 || handle > static_cast<int>(portsIn.size()))
        return false;  // unknown handle: report active, never block reads
    auto& port = portsIn[handle - 1];
    if (!port.isOpen)
        return false;
    if (!Config::getMicGateEnabled(port.user_id - 1))
        return false;
    return port.silent.load(std::memory_order_relaxed);
}

void SDLAudioIn::AudioInClose(int handle) {
    SDL_AudioStream* stream_to_destroy = nullptr;
    AudioInPort* port_ptr = nullptr;
    {
        std::scoped_lock lock{m_mutex};
        if (handle < 1 || handle > static_cast<int>(portsIn.size()))
            return;
        auto& port = portsIn[handle - 1];
        if (!port.isOpen)
            return;
        port_ptr = &port;
        // Take the port's data mutex too — any AudioInInput call still
        // inside the wait loop holds this; we'll get the lock once it
        // returns. Clearing port.stream while we hold it guarantees the
        // waiter sees the null on its next iteration and bails out
        // BEFORE we destroy the underlying SDL stream.
        std::scoped_lock data_lock{*port.data_mu};
        stream_to_destroy = port.stream;
        port.Reset();
    }
    // Wake any pending waiter so it sees stream == null and returns.
    if (port_ptr && port_ptr->data_cv) {
        port_ptr->data_cv->notify_all();
    }
    if (stream_to_destroy) {
        // Clear the callback first so SDL doesn't re-enter our code with
        // a now-dangling port pointer during destruction.
        SDL_SetAudioStreamPutCallback(stream_to_destroy, nullptr, nullptr);
        SDL_DestroyAudioStream(stream_to_destroy);
    }
}
