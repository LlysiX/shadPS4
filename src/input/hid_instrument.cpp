// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// IO layer for legacy raw-HID / XInput instruments. Owns the SDL handles
// and the background polling thread. Pure-logic packing (kit parsing,
// PackDeviceUniqueData, PackButtons, ParseTypedData, etc.) lives in
// hid_packer.cpp so the regression test harness can link against it
// without dragging SDL3 in.

#include "input/hid_instrument.h"
#include "input/hid_kit_def.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hidapi.h>
#include <SDL3/SDL_init.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/config.h"
#include "common/logging/log.h"

namespace OPB = Libraries::Pad;

namespace Input::HidInstrument {

namespace {

std::atomic<bool> g_initialized{false};
std::atomic<bool> g_thread_running{false};
std::thread g_thread;
std::once_flag g_init_once;

void CloseSlot(SlotState& s) {
    if (s.dev) {
        SDL_hid_close(static_cast<SDL_hid_device*>(s.dev));
        s.dev = nullptr;
    }
    if (s.gamepad) {
        SDL_CloseGamepad(static_cast<SDL_Gamepad*>(s.gamepad));
        s.gamepad = nullptr;
    }
    s.vid = s.pid = 0;
    s.device_path.clear();
    s.kit = nullptr;
    std::lock_guard<std::mutex> lk(s.mu);
    s.has_data = false;
    s.last_report_len = 0;
}

// 17-byte synthetic HID-style report we build from an SDL gamepad. Every
// XInput-source TOML reads from these byte offsets:
//   0  face button flags: A=bit0, B=bit1, X=bit2, Y=bit3, LB=bit4, RB=bit5
//   1  Start=bit0, Back=bit1, Guide=bit2, Left/RightStickClick=bit3/4
//   2  D-pad bitmap (up=bit0, down=bit1, left=bit2, right=bit3); neutral=0
//   3  Left  stick X as u8 (0x80 center)
//   4  Left  stick Y as u8
//   5  Right stick X as u8 (= whammy on GH X360 guitar)
//   6  Right stick Y as u8 (= tilt   on GH X360 guitar)
//   7  Left  trigger (u8 0..0xFF)
//   8  Right trigger
//   9-10  Left  stick X as int16 little-endian (low byte first)
//   11-12 Left  stick Y as int16 LE     ← green vel (low), red vel (high)
//   13-14 Right stick X as int16 LE     ← yellow vel (low), blue vel (high)
//   15-16 Right stick Y as int16 LE     ← orange vel (low), kick vel (high)
void FillXInputReport(SDL_Gamepad* gp, u8* out) {
    std::memset(out, 0, kXInputReportLen);
    if (!gp) return;
    auto btn = [&](SDL_GamepadButton b) {
        return SDL_GetGamepadButton(gp, b) ? 1 : 0;
    };
    auto axis_u8 = [&](SDL_GamepadAxis a) -> u8 {
        const int v = SDL_GetGamepadAxis(gp, a);
        int u = (v + 32768) >> 8;
        if (u < 0) u = 0;
        if (u > 255) u = 255;
        return static_cast<u8>(u);
    };
    auto trig_u8 = [&](SDL_GamepadAxis a) -> u8 {
        int v = SDL_GetGamepadAxis(gp, a);
        if (v < 0) v = 0;
        return static_cast<u8>(v >> 7);
    };
    auto axis_i16 = [&](SDL_GamepadAxis a, u8* dst) {
        const int16_t v = static_cast<int16_t>(SDL_GetGamepadAxis(gp, a));
        dst[0] = static_cast<u8>(v & 0xFF);
        dst[1] = static_cast<u8>((v >> 8) & 0xFF);
    };
    out[0] = (btn(SDL_GAMEPAD_BUTTON_SOUTH)          << 0) |
             (btn(SDL_GAMEPAD_BUTTON_EAST)           << 1) |
             (btn(SDL_GAMEPAD_BUTTON_WEST)           << 2) |
             (btn(SDL_GAMEPAD_BUTTON_NORTH)          << 3) |
             (btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)  << 4) |
             (btn(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) << 5);
    out[1] = (btn(SDL_GAMEPAD_BUTTON_START)          << 0) |
             (btn(SDL_GAMEPAD_BUTTON_BACK)           << 1) |
             (btn(SDL_GAMEPAD_BUTTON_GUIDE)          << 2) |
             (btn(SDL_GAMEPAD_BUTTON_LEFT_STICK)     << 3) |
             (btn(SDL_GAMEPAD_BUTTON_RIGHT_STICK)    << 4);
    out[2] = (btn(SDL_GAMEPAD_BUTTON_DPAD_UP)        << 0) |
             (btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN)      << 1) |
             (btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT)      << 2) |
             (btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)     << 3);
    out[3] = axis_u8(SDL_GAMEPAD_AXIS_LEFTX);
    out[4] = axis_u8(SDL_GAMEPAD_AXIS_LEFTY);
    out[5] = axis_u8(SDL_GAMEPAD_AXIS_RIGHTX);
    out[6] = axis_u8(SDL_GAMEPAD_AXIS_RIGHTY);
    out[7] = trig_u8(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    out[8] = trig_u8(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    axis_i16(SDL_GAMEPAD_AXIS_LEFTX,  out + 9);
    axis_i16(SDL_GAMEPAD_AXIS_LEFTY,  out + 11);
    axis_i16(SDL_GAMEPAD_AXIS_RIGHTX, out + 13);
    axis_i16(SDL_GAMEPAD_AXIS_RIGHTY, out + 15);
}

bool GamepadMatchesKit(const KitDef& kd, SDL_JoystickID gpid) {
    const u16 vid = SDL_GetGamepadVendorForID(gpid);
    const u16 pid = SDL_GetGamepadProductForID(gpid);
    if (kd.vid && kd.pid) {
        return kd.vid == vid && kd.pid == pid;
    }
    if (!kd.match_name.empty()) {
        const char* n = SDL_GetGamepadNameForID(gpid);
        if (!n) return false;
        std::string name(n);
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        std::string needle = kd.match_name;
        std::transform(needle.begin(), needle.end(), needle.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return name.find(needle) != std::string::npos;
    }
    return false;
}

void PollLoop() {
    while (g_thread_running.load(std::memory_order_acquire)) {
        for (int i = 0; i < kNumSlots; ++i) {
            const int slot = i + 1;
            const bool enabled = Config::getSpecialPadLegacyPassUSBRawHID(slot);
            SlotState& s = g_slots[i];

            if (!enabled) {
                if (s.dev || s.gamepad) {
                    LOG_INFO(Input, "HID instrument slot {}: flag disabled, closing device",
                             slot);
                    CloseSlot(s);
                }
                continue;
            }

            if (!s.dev && !s.gamepad) {
                std::vector<KitDef> snapshot;
                {
                    std::lock_guard<std::mutex> lk(g_kits_mu);
                    snapshot = g_kits;
                }
                for (const auto& kd : snapshot) {
                    if (kd.source == "xinput") {
                        int gpcount = 0;
                        SDL_JoystickID* gps = SDL_GetGamepads(&gpcount);
                        for (int gi = 0; gi < gpcount && !s.gamepad; ++gi) {
                            if (!GamepadMatchesKit(kd, gps[gi])) continue;
                            const std::string path = "xinput:" + std::to_string(gps[gi]);
                            if (PathInUseByOtherSlot(path, i)) continue;
                            SDL_Gamepad* g = SDL_OpenGamepad(gps[gi]);
                            if (!g) continue;
                            s.gamepad = g;
                            s.vid = SDL_GetGamepadVendor(g);
                            s.pid = SDL_GetGamepadProduct(g);
                            s.device_path = path;
                            s.kit = FindKit(kd.vid, kd.pid);
                            if (!s.kit) s.kit = &kd;
                            s.open_failed_logged = false;
                            LOG_INFO(Input,
                                     "HID instrument slot {}: opened XInput "
                                     "gamepad #{} ({})",
                                     slot, gps[gi], kd.name);
                        }
                        SDL_free(gps);
                    } else {
                        SDL_hid_device_info* head = SDL_hid_enumerate(kd.vid, kd.pid);
                        for (auto* dn = head; dn && !s.dev; dn = dn->next) {
                            const std::string path = dn->path ? dn->path : "";
                            if (path.empty()) continue;
                            if (PathInUseByOtherSlot(path, i)) continue;
                            SDL_hid_device* d = SDL_hid_open_path(path.c_str());
                            if (!d) continue;
                            SDL_hid_set_nonblocking(d, 1);
                            s.dev = d;
                            s.vid = kd.vid;
                            s.pid = kd.pid;
                            s.device_path = path;
                            s.kit = FindKit(kd.vid, kd.pid);
                            s.open_failed_logged = false;
                            LOG_INFO(Input,
                                     "HID instrument slot {}: opened {:04x}:{:04x} "
                                     "({}) at {}",
                                     slot, kd.vid, kd.pid, kd.name, path);
                        }
                        SDL_hid_free_enumeration(head);
                    }
                    if (s.dev || s.gamepad) break;
                }
                if (!s.dev && !s.gamepad && !s.open_failed_logged) {
                    LOG_WARNING(Input,
                                "HID instrument slot {}: no known kit found "
                                "(specialPadLegacyPassUSBRawHID{} is true)",
                                slot, slot);
                    s.open_failed_logged = true;
                }
                continue;
            }

            if (s.gamepad) {
                SDL_UpdateGamepads();
                u8 buf[kXInputReportLen];
                FillXInputReport(static_cast<SDL_Gamepad*>(s.gamepad), buf);
                std::lock_guard<std::mutex> lk(s.mu);
                std::memcpy(s.last_report, buf, kXInputReportLen);
                s.last_report_len = kXInputReportLen;
                s.has_data = true;
            } else if (s.dev) {
                u8 buf[kMaxRawReport];
                int n = SDL_hid_read_timeout(
                    static_cast<SDL_hid_device*>(s.dev), buf, kMaxRawReport, 0);
                if (n > 0) {
                    std::lock_guard<std::mutex> lk(s.mu);
                    std::memcpy(s.last_report, buf, n);
                    s.last_report_len = static_cast<std::size_t>(n);
                    s.has_data = true;
                } else if (n < 0) {
                    LOG_WARNING(Input,
                                "HID instrument slot {}: read error, closing & will retry",
                                slot);
                    CloseSlot(s);
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

}  // namespace

void RescanKits() {
    {
        std::lock_guard<std::mutex> lk(g_kits_mu);
        g_kits.clear();
    }
    LoadAllKits();
    // Drop any per-slot kit pointers so they get re-resolved on the next
    // poll tick (the old pointer was into the cleared g_kits vector).
    for (int i = 0; i < kNumSlots; ++i) {
        SlotState& s = g_slots[i];
        if (s.dev) {
            SDL_hid_close(static_cast<SDL_hid_device*>(s.dev));
            s.dev = nullptr;
        }
        s.vid = s.pid = 0;
        s.device_path.clear();
        s.kit = nullptr;
        std::lock_guard<std::mutex> lk(s.mu);
        s.has_data = false;
        s.last_report_len = 0;
        s.open_failed_logged = false;
    }
}

bool EnsureInit() {
    bool ok = true;
    std::call_once(g_init_once, [&] {
        // Use the shared lazy loader so the kit list is populated exactly
        // once, regardless of whether ShouldHideFromUsbd() or EnsureInit()
        // wins the race during game boot.
        EnsureKitsLoaded();
        if (SDL_hid_init() != 0) {
            LOG_ERROR(Input, "SDL_hid_init failed");
            ok = false;
            return;
        }
        SDL_InitSubSystem(SDL_INIT_GAMEPAD);
        g_thread_running.store(true, std::memory_order_release);
        g_thread = std::thread(PollLoop);
        g_initialized.store(true, std::memory_order_release);
    });
    return ok && g_initialized.load(std::memory_order_acquire);
}

void Shutdown() {
    if (!g_initialized.exchange(false)) {
        return;
    }
    g_thread_running.store(false, std::memory_order_release);
    if (g_thread.joinable()) {
        g_thread.join();
    }
    for (auto& s : g_slots) {
        CloseSlot(s);
    }
    SDL_hid_exit();
}

std::vector<XInputDeviceInfo> EnumerateXInputDevices() {
    std::vector<XInputDeviceInfo> out;
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    if (!ids) return out;
    for (int i = 0; i < n; ++i) {
        XInputDeviceInfo info;
        info.instance_id = ids[i];
        info.vendor_id = SDL_GetGamepadVendorForID(ids[i]);
        info.product_id = SDL_GetGamepadProductForID(ids[i]);
        const char* nm = SDL_GetGamepadNameForID(ids[i]);
        info.name = nm ? nm : "";
        out.push_back(std::move(info));
    }
    SDL_free(ids);
    return out;
}

void* OpenXInputGamepad(int instance_id) {
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    return SDL_OpenGamepad(instance_id);
}

void CloseXInputGamepad(void* gamepad) {
    if (gamepad) SDL_CloseGamepad(static_cast<SDL_Gamepad*>(gamepad));
}

void PollXInputGamepad(void* gamepad, u8* out) {
    SDL_UpdateGamepads();
    FillXInputReport(static_cast<SDL_Gamepad*>(gamepad), out);
}

}  // namespace Input::HidInstrument
