// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <SDL3/SDL.h>

namespace Libraries::AudioIn {
enum OrbisAudioInParam {
    ORBIS_AUDIO_IN_PARAM_FORMAT_S16_MONO = 0,
    ORBIS_AUDIO_IN_PARAM_FORMAT_S16_STEREO = 2
};
}

#define ORBIS_AUDIO_IN_ERROR_INVALID_PORT -1
#define ORBIS_AUDIO_IN_ERROR_TIMEOUT -2
#define ORBIS_AUDIO_IN_ERROR_STREAM_FAIL -3

class SDLAudioIn {
public:
    int AudioInit();
    int AudioInOpen(int type, uint32_t samples_num, uint32_t freq, uint32_t format);
    int AudioInInput(int handle, void* out_buffer);
    void AudioInClose(int handle);
    // True when the software noise gate currently considers the port's
    // input silent. sceAudioInGetSilentState reads this so the game can
    // skip its vocal mix while the user isn't talking. Returns true
    // (silent) for an unopened/invalid handle.
    bool IsSilent(int handle);

private:
    // Per-port state. Lives in the fixed-size portsIn array so the pointer
    // we hand to SDL_SetAudioStreamPutCallback as userdata is stable for
    // the lifetime of the SDLAudioIn instance. The mutex/cv are
    // unique_ptr-wrapped because std::mutex / std::condition_variable
    // aren't movable or assignable — AudioInClose used to do `port = {}`
    // to reset the slot; that path now goes through Reset() instead.
    struct AudioInPort {
        bool isOpen = false;
        int type = 0;
        uint32_t samples_num = 0;
        uint32_t freq = 0;
        int channels_num = 0;
        int sample_size = 0;
        uint32_t format = 0;
        SDL_AudioStream* stream = nullptr;
        // Signalled by the SDL put-callback whenever the capture device
        // pushes a new chunk into the stream. AudioInInput waits on this
        // CV (with a short timeout) instead of busy-polling SDL_Delay(1).
        std::unique_ptr<std::mutex> data_mu = std::make_unique<std::mutex>();
        std::unique_ptr<std::condition_variable> data_cv =
            std::make_unique<std::condition_variable>();

        // Software noise gate. gate_open tracks whether the gate is
        // currently passing audio; last_active is the last time the
        // input level exceeded the threshold (used for the hangover so
        // the tail of a word isn't chopped). silent mirrors the gate
        // for sceAudioInGetSilentState — atomic because that query can
        // come from a different thread than AudioInInput.
        bool gate_open = false;
        std::chrono::steady_clock::time_point last_active{};
        std::atomic<bool> silent{true};

        void Reset() {
            isOpen = false;
            type = 0;
            samples_num = 0;
            freq = 0;
            channels_num = 0;
            sample_size = 0;
            format = 0;
            stream = nullptr;
            gate_open = false;
            last_active = {};
            silent.store(true, std::memory_order_relaxed);
            // mutex / cv are left in place — they're recycled when the
            // slot is reopened.
        }
    };

    std::array<AudioInPort, 8> portsIn;
    // Guards portsIn integrity (slot allocation, open/close). The wait
    // inside AudioInInput uses each port's own mutex so a slow drain on
    // one port doesn't stall opens/closes on another.
    std::mutex m_mutex;

    static void SDLCALL OnStreamPut(void* userdata, SDL_AudioStream* stream, int additional_amount,
                                    int total_amount);
};
