// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Internal types shared between the runtime IO layer (hid_instrument.cpp,
// pulls in SDL3) and the pure-logic packer (hid_packer.cpp, no SDL deps).
// The split keeps the regression test harness off SDL — it compiles and
// runs on every platform without needing the gamepad or hidraw stack.

#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "common/types.h"
#include "input/hid_instrument.h"

namespace Input::HidInstrument {

// Data-driven description of a legacy instrument, populated from TOML.
struct KitDef {
    u16 vid = 0;
    u16 pid = 0;
    std::string name;
    std::string device_class;
    // "hid" or "xinput" — the runtime IO layer branches on this.
    std::string source = "hid";
    // Case-insensitive name substring used to match XInput gamepads when
    // vid/pid are 0. Lets a single TOML claim every "Guitar Hero" pad.
    std::string match_name;
    std::size_t report_length = 0;
    // dud_layout[i] = raw byte index for deviceUniqueData[i], or -1 for zero.
    std::array<int, kMaxDeviceUniqueData> dud_layout{-1, -1, -1, -1, -1, -1,
                                                     -1, -1, -1, -1, -1, -1};
    // raw[byte] bits -> Orbis button bitmask, for any raw byte index.
    std::map<int, std::array<u32, 8>> button_bytes;
    int hat_byte = -1;
    int tilt_byte = -1;
    int tilt_byte_high = -1;
    int tilt_baseline = 0x80;
    int tilt_scale = 90;
    int whammy_byte = -1;
    int touch_byte = -1;
    int tone_byte = -1;
    int whammy_baseline = 0x80;
    bool tilt_invert = false;
    int fret_byte = 0;
    u8 fret_mask = 0xFF;
    int solo_fret_byte = -1;
    // Some guitars (Xbox 360 RB, original Stratocaster) don't have a
    // dedicated solo-fret bitmask — they encode "solo press" as "main fret
    // bit held while a modifier button (left-stick click on X360) is
    // also held". When solo_modifier_byte/mask are set the packer routes
    // the main fret value to dud[4] instead of dud[3] for the duration
    // the modifier is pressed.
    int solo_modifier_byte = -1;
    u8 solo_modifier_mask = 0;
    bool guitar_ps4_layout = false;
    bool drum_ps4_layout = false;
    int drum_red_byte           = -1;
    int drum_blue_byte          = -1;
    int drum_yellow_byte        = -1;
    int drum_green_byte         = -1;
    int drum_yellow_cymbal_byte = -1;
    int drum_blue_cymbal_byte   = -1;
    int drum_green_cymbal_byte  = -1;
    std::vector<int> motion_bytes;
    u8 clear_dud0_when_raw1_bits = 0;
    bool has_dud0_remap = false;
    std::array<int, 8> dud0_bit_remap{0, 1, 2, 3, 4, 5, 6, 7};
    std::array<int, kMaxDeviceUniqueData> dud_scale_lo{};
    std::array<int, kMaxDeviceUniqueData> dud_scale_hi{};
    std::string source_file;
};

// SDL handles are kept as void* so this header doesn't need SDL3 headers.
// The IO layer (hid_instrument.cpp) casts them to SDL_hid_device* /
// SDL_Gamepad*; the packer never touches them.
struct SlotState {
    void* dev = nullptr;
    void* gamepad = nullptr;
    u16 vid = 0, pid = 0;
    std::string device_path;
    const KitDef* kit = nullptr;
    std::mutex mu;
    u8 last_report[kMaxRawReport]{};
    std::size_t last_report_len = 0;
    bool has_data = false;
    bool open_failed_logged = false;
};

// Defined in hid_packer.cpp; the IO layer reads them.
extern std::vector<KitDef> g_kits;
extern std::mutex g_kits_mu;
extern SlotState g_slots[kNumSlots];

// Pure-logic helpers exposed for the IO layer + tests. Defined in hid_packer.cpp.
bool LoadKitFromToml(const std::string& file_path);
void LoadAllKits();
// Lazy no-SDL version — populates g_kits exactly once across the process.
// Both EnsureInit() (IO layer) and ShouldHideFromUsbd() (libusb shim) call
// it; whoever runs first wins the race, the other becomes a no-op. This
// prevents the "device shows up twice" failure when the game enumerates
// USB devices before its first scePadRead.
void EnsureKitsLoaded();
const KitDef* FindKit(u16 vid, u16 pid);
bool PathInUseByOtherSlot(const std::string& path, int this_slot_index);
constexpr u8 ScaleVel7to8(u8 v) {
    const u8 v7 = v & 0x7F;
    return static_cast<u8>((v7 << 1) | (v7 >> 6));
}

}  // namespace Input::HidInstrument
