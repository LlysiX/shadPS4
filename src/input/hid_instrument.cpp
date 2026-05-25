// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "input/hid_instrument.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hidapi.h>
#include <SDL3/SDL_init.h>
#include <toml.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/config.h"
#include "common/logging/log.h"
#include "common/path_util.h"

namespace fs = std::filesystem;
namespace OPB = Libraries::Pad;

namespace Input::HidInstrument {

// Data-driven description of a legacy instrument, populated from TOML.
struct KitDef {
    u16 vid = 0;
    u16 pid = 0;
    std::string name;
    std::string device_class;
    // "hid" (default) — kit is read via SDL_hid from its USB HID report.
    // "xinput" — kit is an Xbox 360 device read via SDL_GameController; we
    // synthesise a 9-byte buffer (face flags / dpad / sticks / triggers)
    // and feed it through the same packer pipeline.
    std::string source = "hid";
    // Optional case-insensitive substring matched against the gamepad's
    // name when source = "xinput" and vid/pid are 0. Lets a single TOML
    // claim e.g. all "Guitar Hero" controllers without a fixed VID:PID.
    std::string match_name;
    std::size_t report_length = 0;
    // dud_layout[i] = raw byte index for deviceUniqueData[i], or -1 for zero.
    std::array<int, kMaxDeviceUniqueData> dud_layout{-1, -1, -1, -1, -1, -1,
                                                     -1, -1, -1, -1, -1, -1};
    // raw[byte] bits -> Orbis button bitmask, for any raw byte index.
    // PS3 GH/RB guitars put face buttons in byte 0; PS4 RB guitars and the
    // PS5 Riffmaster put them in bytes 8/9 (Riffmaster) or 5/6 (PS4 RB).
    std::map<int, std::array<u32, 8>> button_bytes;
    int hat_byte = -1;
    int tilt_byte = -1;
    int tilt_byte_high = -1;
    int tilt_baseline = 0x80;
    int tilt_scale = 90;
    int whammy_byte = -1;
    int touch_byte = -1;
    int tone_byte = -1;
    // Whammy raw value at "no input". PS3 GH guitars rest at 0x80 (centered
    // axis, push-only to 0xFF). PS4 RB and PS5 Riffmaster rest at 0x00 and
    // push to 0xFF (unsigned ramp).
    int whammy_baseline = 0x80;
    // Set true when the tilt byte's polarity is reversed from PS3 GH (i.e.
    // raw INCREASES when the guitar is tilted up). PS5 Riffmaster uses
    // this convention; PS3 GH does not.
    bool tilt_invert = false;
    // Source byte for the fret bitmap in guitar_ps4_layout mode. PS3 guitars
    // = 0 (face button byte), PS4 RB = 46, PS5 Riffmaster = 43.
    int fret_byte = 0;
    // Mask applied to fret_byte before bit-remap. PS4 RB and PS5 Riffmaster
    // share the byte with HAT data in the low nibble; without masking, the
    // 0x0F neutral HAT value remaps into "all frets held" in dud[3].
    u8 fret_mask = 0xFF;
    // Source byte for the solo (upper) fret bitmap, or -1 if absent.
    int solo_fret_byte = -1;
    // Emit dud[] in PS4 RB guitar layout (pickup/whammy/tilt/frets/solo).
    bool guitar_ps4_layout = false;
    // Emit dud[] in PS4 RB Pro drum layout (pads 0..3, cymbals 4..6).
    bool drum_ps4_layout = false;
    int drum_red_byte           = -1;
    int drum_blue_byte          = -1;
    int drum_yellow_byte        = -1;
    int drum_green_byte         = -1;
    int drum_yellow_cymbal_byte = -1;
    int drum_blue_cymbal_byte   = -1;
    int drum_green_cymbal_byte  = -1;
    // Sensor bytes flagged during baseline — excluded from the velocity
    // heuristic on future probes.
    std::vector<int> motion_bytes;
    // When raw[1] & this is non-zero, dud[0] is forced to 0 (suppresses
    // stray fret/pad bits while menu inputs are held).
    u8 clear_dud0_when_raw1_bits = 0;
    // PS3 guitars have a non-standard fret bit order; this remaps each
    // set bit in raw[0] to the position RB4 expects in dud[0].
    bool has_dud0_remap = false;
    std::array<int, 8> dud0_bit_remap{0, 1, 2, 3, 4, 5, 6, 7};
    // Per-dud linear velocity rescaling. When hi > lo, raw [lo..hi] maps
    // onto [0..0x7F] before being written. Compensates for worn pads.
    std::array<int, kMaxDeviceUniqueData> dud_scale_lo{};
    std::array<int, kMaxDeviceUniqueData> dud_scale_hi{};
    std::string source_file;
};

namespace {

std::vector<KitDef> g_kits;
std::mutex g_kits_mu;

u32 ButtonByName(std::string_view name) {
    using B = OPB::OrbisPadButtonDataOffset;
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    static const std::pair<const char*, B> map[] = {
        {"square", B::Square},     {"cross", B::Cross},
        {"circle", B::Circle},     {"triangle", B::Triangle},
        {"l1", B::L1},             {"r1", B::R1},
        {"l2", B::L2},             {"r2", B::R2},
        {"l3", B::L3},             {"r3", B::R3},
        {"options", B::Options},   {"touchpad", B::TouchPad},
        {"up", B::Up},             {"down", B::Down},
        {"left", B::Left},         {"right", B::Right},
    };
    for (const auto& [n, v] : map) {
        if (lower == n) return static_cast<u32>(v);
    }
    return 0;
}

u32 ParseHexOrDec(const std::string& s) {
    try {
        if (s.size() > 2 && (s[0] == '0') && (s[1] == 'x' || s[1] == 'X')) {
            return static_cast<u32>(std::stoul(s.substr(2), nullptr, 16));
        }
        return static_cast<u32>(std::stoul(s, nullptr, 10));
    } catch (...) {
        return 0;
    }
}

bool LoadKitFromToml(const fs::path& file) {
    try {
        const toml::value root = toml::parse(file.string());

        KitDef k;
        k.source_file = file.string();
        k.vid = static_cast<u16>(ParseHexOrDec(toml::find<std::string>(root, "vendor_id")));
        k.pid = static_cast<u16>(ParseHexOrDec(toml::find<std::string>(root, "product_id")));
        if (k.vid == 0 || k.pid == 0) {
            LOG_WARNING(Input, "kit {}: missing/invalid vendor_id or product_id",
                        file.filename().string());
            return false;
        }
        k.name = toml::find_or<std::string>(root, "name", "(unnamed)");
        k.device_class = toml::find_or<std::string>(root, "device_class", "");
        k.source = toml::find_or<std::string>(root, "source", "hid");
        k.match_name = toml::find_or<std::string>(root, "match_name", "");
        k.report_length = static_cast<std::size_t>(toml::find_or<int>(root, "report_length", 0));
        k.hat_byte = toml::find_or<int>(root, "hat_byte", -1);
        k.tilt_byte = toml::find_or<int>(root, "tilt_byte", -1);
        k.tilt_byte_high = toml::find_or<int>(root, "tilt_byte_high", -1);
        k.tilt_baseline = toml::find_or<int>(root, "tilt_baseline", 0x80);
        k.tilt_scale = toml::find_or<int>(root, "tilt_scale", 90);
        k.whammy_byte = toml::find_or<int>(root, "whammy_byte", -1);
        k.touch_byte = toml::find_or<int>(root, "touch_byte", -1);
        k.tone_byte = toml::find_or<int>(root, "tone_byte", -1);
        k.fret_byte = toml::find_or<int>(root, "fret_byte", 0);
        k.fret_mask = static_cast<u8>(toml::find_or<int>(root, "fret_mask", 0xFF));
        k.solo_fret_byte = toml::find_or<int>(root, "solo_fret_byte", -1);
        k.whammy_baseline = toml::find_or<int>(root, "whammy_baseline", 0x80);
        k.tilt_invert = toml::find_or<bool>(root, "tilt_invert", false);
        k.guitar_ps4_layout = toml::find_or<bool>(root, "guitar_ps4_layout", false);
        k.drum_ps4_layout = toml::find_or<bool>(root, "drum_ps4_layout", false);
        k.drum_red_byte           = toml::find_or<int>(root, "drum_red_byte", -1);
        k.drum_blue_byte          = toml::find_or<int>(root, "drum_blue_byte", -1);
        k.drum_yellow_byte        = toml::find_or<int>(root, "drum_yellow_byte", -1);
        k.drum_green_byte         = toml::find_or<int>(root, "drum_green_byte", -1);
        k.drum_yellow_cymbal_byte = toml::find_or<int>(root, "drum_yellow_cymbal_byte", -1);
        k.drum_blue_cymbal_byte   = toml::find_or<int>(root, "drum_blue_cymbal_byte", -1);
        k.drum_green_cymbal_byte  = toml::find_or<int>(root, "drum_green_cymbal_byte", -1);
        if (root.contains("motion_bytes")) {
            k.motion_bytes = toml::find<std::vector<int>>(root, "motion_bytes");
        }
        if (root.contains("dud0_bit_remap")) {
            const auto& arr = toml::find<std::vector<int>>(root, "dud0_bit_remap");
            for (std::size_t i = 0; i < 8 && i < arr.size(); ++i) {
                k.dud0_bit_remap[i] = arr[i];
            }
            k.has_dud0_remap = true;
        }
        k.clear_dud0_when_raw1_bits = static_cast<u8>(
            toml::find_or<int>(root, "clear_dud0_when_raw1_bits", 0));

        if (root.contains("device_unique_data")) {
            const auto& arr = toml::find<std::vector<int>>(root, "device_unique_data");
            for (std::size_t i = 0; i < kMaxDeviceUniqueData && i < arr.size(); ++i) {
                k.dud_layout[i] = arr[i];
            }
        }

        // Accept [buttons_byte_N] for any non-negative byte index. Each
        // section maps raw[N]'s bit pattern to PS4 button names.
        auto load_button_table = [&](const std::string& section, int byte_idx) {
            const auto& tbl = toml::find(root, section).as_table();
            auto& out = k.button_bytes[byte_idx];
            for (const auto& [key, val] : tbl) {
                const u32 mask = ParseHexOrDec(key);
                const u32 btn = ButtonByName(val.as_string());
                for (int bit = 0; bit < 8; ++bit) {
                    if (mask & (1u << bit)) out[bit] |= btn;
                }
            }
        };
        if (root.is_table()) {
            for (const auto& [key, val] : root.as_table()) {
                if (key.rfind("buttons_byte_", 0) != 0) continue;
                int byte_idx = 0;
                try {
                    byte_idx = std::stoi(key.substr(13));
                } catch (...) { continue; }
                if (byte_idx < 0) continue;
                load_button_table(key, byte_idx);
            }
        }

        if (root.contains("velocity_scaling")) {
            const auto& tbl = toml::find(root, "velocity_scaling").as_table();
            for (const auto& [key, val] : tbl) {
                int idx = 0;
                try { idx = std::stoi(key); } catch (...) { continue; }
                if (idx < 0 || idx >= (int)kMaxDeviceUniqueData) continue;
                if (!val.is_table()) continue;
                const auto& o = val.as_table();
                if (o.count("lo")) k.dud_scale_lo[idx] = o.at("lo").as_integer();
                if (o.count("hi")) k.dud_scale_hi[idx] = o.at("hi").as_integer();
            }
        }

        std::lock_guard<std::mutex> lk(g_kits_mu);
        auto it = std::find_if(g_kits.begin(), g_kits.end(),
                               [&](const KitDef& e) { return e.vid == k.vid && e.pid == k.pid; });
        if (it != g_kits.end()) {
            *it = std::move(k);
            LOG_INFO(Input, "kit reloaded from {} ({:04x}:{:04x})",
                     file.filename().string(), it->vid, it->pid);
        } else {
            LOG_INFO(Input, "kit loaded from {}: {} ({:04x}:{:04x})",
                     file.filename().string(), k.name, k.vid, k.pid);
            g_kits.push_back(std::move(k));
        }
        return true;
    } catch (const std::exception& e) {
        LOG_WARNING(Input, "kit {}: parse error: {}", file.filename().string(), e.what());
        return false;
    }
}

void LoadAllKits() {
    // Recursive so users can organise kits under subfolders
    // (e.g. <UserDir>/kits/comkits/ for community-contributed defs).
    auto scan_dir = [](const fs::path& dir) {
        std::error_code ec;
        if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return;
        for (auto it = fs::recursive_directory_iterator(
                 dir, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec) && it->path().extension() == ".toml") {
                LoadKitFromToml(it->path());
            }
        }
    };
    scan_dir(fs::current_path() / "scripts" / "kits");
    try {
        const auto user_kits = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "kits";
        scan_dir(user_kits);
    } catch (...) {
    }
}

const KitDef* FindKit(u16 vid, u16 pid) {
    std::lock_guard<std::mutex> lk(g_kits_mu);
    for (const auto& k : g_kits) {
        if (k.vid == vid && k.pid == pid) return &k;
    }
    return nullptr;
}

constexpr u8 ScaleVel7to8(u8 v) {
    const u8 v7 = v & 0x7F;
    return static_cast<u8>((v7 << 1) | (v7 >> 6));
}

struct SlotState {
    // One of dev (HID) or gamepad (XInput) is set; never both.
    SDL_hid_device* dev = nullptr;
    SDL_Gamepad* gamepad = nullptr;
    u16 vid = 0, pid = 0;
    std::string device_path;
    const KitDef* kit = nullptr;
    std::mutex mu;
    u8 last_report[kMaxRawReport]{};
    std::size_t last_report_len = 0;
    bool has_data = false;
    bool open_failed_logged = false;
};

SlotState g_slots[kNumSlots];
std::atomic<bool> g_initialized{false};
std::atomic<bool> g_thread_running{false};
std::thread g_thread;
std::once_flag g_init_once;

void CloseSlot(SlotState& s) {
    if (s.dev) {
        SDL_hid_close(s.dev);
        s.dev = nullptr;
    }
    if (s.gamepad) {
        SDL_CloseGamepad(s.gamepad);
        s.gamepad = nullptr;
    }
    s.vid = s.pid = 0;
    s.device_path.clear();
    s.kit = nullptr;
    std::lock_guard<std::mutex> lk(s.mu);
    s.has_data = false;
    s.last_report_len = 0;
}

bool PathInUseByOtherSlot(const std::string& path, int this_slot_index) {
    for (int i = 0; i < kNumSlots; ++i) {
        if (i == this_slot_index) continue;
        if ((g_slots[i].dev || g_slots[i].gamepad) &&
            g_slots[i].device_path == path) {
            return true;
        }
    }
    return false;
}

// 17-byte synthetic HID-style report we build from an SDL gamepad. Every
// XInput-source TOML reads from these byte offsets:
//   0  face button flags: A=bit0, B=bit1, X=bit2, Y=bit3, LB=bit4, RB=bit5
//   1  Start=bit0, Back=bit1, Guide=bit2, Left/RightStickClick=bit3/4
//   2  D-pad bitmap (up=bit0, down=bit1, left=bit2, right=bit3); neutral=0
//   3  Left  stick X as u8 (0x80 center) — convenient for analog axes
//   4  Left  stick Y as u8
//   5  Right stick X as u8 (= whammy on GH X360 guitar)
//   6  Right stick Y as u8 (= tilt   on GH X360 guitar)
//   7  Left  trigger (u8 0..0xFF)
//   8  Right trigger
//   9-10  Left  stick X as int16 little-endian (low byte first)
//   11-12 Left  stick Y as int16 LE     ← green vel (low), red vel (high)
//   13-14 Right stick X as int16 LE     ← yellow vel (low), blue vel (high)
//   15-16 Right stick Y as int16 LE     ← orange vel (low), kick vel (high)
// Per-byte access lets X360 GH drum kits address the velocity bytes
// independently (they pack a different colour's MIDI velocity into each
// half of every stick axis — see PlasticBand 5-Lane Drums/Xbox 360.md).
// (kXInputReportLen lives in the header so the wizard can use it too.)

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
        // SDL_GetGamepadAxis is already int16-range; write little-endian.
        const int16_t v = static_cast<int16_t>(SDL_GetGamepadAxis(gp, a));
        dst[0] = static_cast<u8>(v & 0xFF);
        dst[1] = static_cast<u8>((v >> 8) & 0xFF);
    };
    out[0] = (btn(SDL_GAMEPAD_BUTTON_SOUTH)          << 0) |  // A
             (btn(SDL_GAMEPAD_BUTTON_EAST)           << 1) |  // B
             (btn(SDL_GAMEPAD_BUTTON_WEST)           << 2) |  // X
             (btn(SDL_GAMEPAD_BUTTON_NORTH)          << 3) |  // Y
             (btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)  << 4) |  // LB
             (btn(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) << 5);   // RB
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
                            if (!s.kit) s.kit = &kd;  // match-by-name kits
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
                // SDL caches gamepad state between events; pump the queue
                // ourselves since we're on a background polling thread.
                SDL_UpdateGamepads();
                u8 buf[kXInputReportLen];
                FillXInputReport(s.gamepad, buf);
                std::lock_guard<std::mutex> lk(s.mu);
                std::memcpy(s.last_report, buf, kXInputReportLen);
                s.last_report_len = kXInputReportLen;
                s.has_data = true;
            } else if (s.dev) {
                u8 buf[kMaxRawReport];
                int n = SDL_hid_read_timeout(s.dev, buf, kMaxRawReport, 0);
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
            SDL_hid_close(s.dev);
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
        LoadAllKits();
        if (SDL_hid_init() != 0) {
            LOG_ERROR(Input, "SDL_hid_init failed");
            ok = false;
            return;
        }
        // Best-effort gamepad init for XInput-source kits. Failure is
        // non-fatal — HID-source kits still work.
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

bool GetLatestReport(int slot, u8* out, std::size_t* out_len) {
    if (slot < 1 || slot > kNumSlots) {
        return false;
    }
    if (!Config::getSpecialPadLegacyPassUSBRawHID(slot)) {
        return false;
    }
    EnsureInit();
    SlotState& s = g_slots[slot - 1];
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.has_data || s.last_report_len == 0) {
        return false;
    }
    if (out) {
        std::memcpy(out, s.last_report, s.last_report_len);
    }
    if (out_len) {
        *out_len = s.last_report_len;
    }
    return true;
}

bool GetLatestAcceleration(int slot, float& out_x, float& out_y, float& out_z) {
    out_x = 0.0f;
    out_y = 0.0f;
    out_z = 0.0f;

    if (slot < 1 || slot > kNumSlots) {
        return false;
    }
    if (!Config::getSpecialPadLegacyPassUSBRawHID(slot)) {
        return false;
    }
    EnsureInit();
    SlotState& s = g_slots[slot - 1];
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.has_data || s.last_report_len == 0 || !s.kit) {
        return false;
    }

    const KitDef* kit = s.kit;
    if (kit->tilt_byte < 0 ||
        static_cast<std::size_t>(kit->tilt_byte) >= s.last_report_len) {
        return false;
    }

    // PS3 GH/RB accel.x is 10-bit, split low + high little-endian.
    int raw = s.last_report[kit->tilt_byte];
    if (kit->tilt_byte_high >= 0 &&
        static_cast<std::size_t>(kit->tilt_byte_high) < s.last_report_len) {
        raw |= (s.last_report[kit->tilt_byte_high] & 0x03) << 8;
    }
    const int delta = raw - kit->tilt_baseline;
    const int scale = (kit->tilt_scale > 0) ? kit->tilt_scale : 128;
    // Default convention: lower raw = pointed up (PS3 GH spec — accelX drops
    // below 0x200 when the guitar lifts). PS5 Riffmaster inverts this — raw
    // goes 0x00..0xFF as the guitar tilts up — so tilt_invert flips the sign.
    float x = -static_cast<float>(delta) / static_cast<float>(scale);
    if (kit->tilt_invert) x = -x;
    if (x < -1.0f) x = -1.0f;
    if (x > 1.0f) x = 1.0f;
    out_x = x;
    return true;
}

std::size_t PackDeviceUniqueData(int slot, const u8* raw, std::size_t raw_len,
                                 OPB::OrbisPadDeviceClass dev_class,
                                 u8 out[kMaxDeviceUniqueData]) {
    (void)dev_class;
    if (!raw || !out || raw_len == 0) {
        return 0;
    }
    const KitDef* kit = nullptr;
    if (slot >= 1 && slot <= 4) {
        kit = g_slots[slot - 1].kit;
    }
    if (!kit) {
        std::memset(out, 0, kMaxDeviceUniqueData);
        return kMaxDeviceUniqueData;
    }

    auto at = [&](int i) -> u8 {
        if (i < 0 || static_cast<std::size_t>(i) >= raw_len) return 0;
        return raw[i];
    };

    // PS4 RB 4-lane Pro drum layout. Velocities scaled MIDI 0..0x7F → 0..0xFF.
    if (kit->drum_ps4_layout) {
        std::memset(out, 0, kMaxDeviceUniqueData);
        auto pack_vel = [&](int dud_idx, int raw_idx) {
            if (raw_idx < 0 || static_cast<std::size_t>(raw_idx) >= raw_len) return;
            u8 v = raw[raw_idx];
            const int lo = kit->dud_scale_lo[dud_idx];
            const int hi = kit->dud_scale_hi[dud_idx];
            if (hi > lo) {
                int scaled = (static_cast<int>(v) - lo) * 0x7F / (hi - lo);
                if (scaled < 0) scaled = 0;
                if (scaled > 0x7F) scaled = 0x7F;
                v = static_cast<u8>(scaled);
            }
            out[dud_idx] = ScaleVel7to8(v);
        };
        pack_vel(0, kit->drum_red_byte);
        pack_vel(1, kit->drum_blue_byte);
        pack_vel(2, kit->drum_yellow_byte);
        pack_vel(3, kit->drum_green_byte);
        pack_vel(4, kit->drum_yellow_cymbal_byte);
        pack_vel(5, kit->drum_blue_cymbal_byte);
        pack_vel(6, kit->drum_green_cymbal_byte);
        return kMaxDeviceUniqueData;
    }

    // PS4 RB guitar layout (PlasticBand spec):
    //   dud[0]=pickup  dud[1]=whammy  dud[2]=tilt  dud[3]=frets  dud[4]=solo
    if (kit->guitar_ps4_layout) {
        std::memset(out, 0, kMaxDeviceUniqueData);
        if (kit->tone_byte >= 0) {
            const u8 tone_raw = at(kit->tone_byte);
            if (tone_raw > 0x10) {
                u8 pos = static_cast<u8>(1 + ((tone_raw - 0x10) * 4 / 0xF0));
                if (pos > 4) pos = 4;
                out[0] = pos;
            }
        }
        if (kit->whammy_byte >= 0) {
            const u8 w = at(kit->whammy_byte);
            const int base = kit->whammy_baseline;
            const int range = std::max(1, 0xFF - base);
            out[1] = (w > base)
                ? static_cast<u8>(std::min(0xFE, (w - base) * 0xFE / range))
                : 0;
        }
        float acc_x = 0.0f, acc_y = 0.0f, acc_z = 0.0f;
        if (GetLatestAcceleration(slot, acc_x, acc_y, acc_z) && acc_x > 0.0f) {
            float t = acc_x * 255.0f;
            if (t > 255.0f) t = 255.0f;
            out[2] = static_cast<u8>(t);
        }
        u8 frets = at(kit->fret_byte) & kit->fret_mask;
        if (kit->clear_dud0_when_raw1_bits && (at(1) & kit->clear_dud0_when_raw1_bits)) {
            frets = 0;
        }
        if (kit->has_dud0_remap) {
            u8 remapped = 0;
            for (int b = 0; b < 8; ++b) {
                if (frets & (1u << b)) {
                    remapped |= (1u << kit->dud0_bit_remap[b]);
                }
            }
            frets = remapped;
        }
        out[3] = frets;
        if (kit->solo_fret_byte >= 0 &&
            static_cast<std::size_t>(kit->solo_fret_byte) < raw_len) {
            out[4] = at(kit->solo_fret_byte);
        }
        return kMaxDeviceUniqueData;
    }

    u8 flags = (kit->dud_layout[0] >= 0) ? at(kit->dud_layout[0]) : 0;
    if (kit->clear_dud0_when_raw1_bits && (at(1) & kit->clear_dud0_when_raw1_bits)) {
        flags = 0;
    }
    if (kit->has_dud0_remap) {
        u8 remapped = 0;
        for (int b = 0; b < 8; ++b) {
            if (flags & (1u << b)) {
                remapped |= (1u << kit->dud0_bit_remap[b]);
            }
        }
        flags = remapped;
    }
    out[0] = flags;
    for (std::size_t i = 1; i < kMaxDeviceUniqueData; ++i) {
        u8 v = at(kit->dud_layout[i]);
        const int lo = kit->dud_scale_lo[i];
        const int hi = kit->dud_scale_hi[i];
        if (hi > lo) {
            int scaled = (static_cast<int>(v) - lo) * 0x7F / (hi - lo);
            if (scaled < 0) scaled = 0;
            if (scaled > 0x7F) scaled = 0x7F;
            v = static_cast<u8>(scaled);
        }
        out[i] = v;
    }
    return kMaxDeviceUniqueData;
}

u32 PackButtons(int slot, const u8* raw, std::size_t raw_len,
                OPB::OrbisPadDeviceClass dev_class) {
    using B = OPB::OrbisPadButtonDataOffset;
    (void)dev_class;
    if (!raw || raw_len == 0) {
        return 0;
    }
    const KitDef* kit = nullptr;
    if (slot >= 1 && slot <= 4) {
        kit = g_slots[slot - 1].kit;
    }
    if (!kit) return 0;

    u32 out = 0;
    for (const auto& [byte_idx, table] : kit->button_bytes) {
        if (byte_idx < 0 || static_cast<std::size_t>(byte_idx) >= raw_len) continue;
        const u8 b = raw[byte_idx];
        for (int bit = 0; bit < 8; ++bit) {
            if (b & (1u << bit)) out |= table[bit];
        }
    }
    if (kit->hat_byte >= 0 && static_cast<std::size_t>(kit->hat_byte) < raw_len) {
        // PS4 RB and PS5 Riffmaster share the HAT byte with fret-flag bits in
        // the upper nibble. Mask before switching so a strum-while-holding-
        // a-fret doesn't get lost (raw byte = 0x2F = green-held-strum-up,
        // which the unmasked switch routed to `default`).
        switch (raw[kit->hat_byte] & 0x0F) {
        case 0x00: out |= static_cast<u32>(B::Up); break;
        case 0x01: out |= static_cast<u32>(B::Up) | static_cast<u32>(B::Right); break;
        case 0x02: out |= static_cast<u32>(B::Right); break;
        case 0x03: out |= static_cast<u32>(B::Right) | static_cast<u32>(B::Down); break;
        case 0x04: out |= static_cast<u32>(B::Down); break;
        case 0x05: out |= static_cast<u32>(B::Down) | static_cast<u32>(B::Left); break;
        case 0x06: out |= static_cast<u32>(B::Left); break;
        case 0x07: out |= static_cast<u32>(B::Left) | static_cast<u32>(B::Up); break;
        default: break;
        }
    }
    return out;
}

bool ParseTypedData(int slot, const u8* dud, std::size_t dud_len,
                    OPB::OrbisPadDeviceClass dev_class,
                    OPB::OrbisPadDeviceClassData* out) {
    if (!dud || !out || dud_len < 8) {
        return false;
    }
    std::memset(out, 0, sizeof(*out));
    out->deviceClass = dev_class;
    out->bDataValid = true;

    if (dev_class == OPB::OrbisPadDeviceClass::Drum) {
        const KitDef* kit = (slot >= 1 && slot <= kNumSlots) ? g_slots[slot - 1].kit
                                                              : nullptr;
        if (kit && kit->drum_ps4_layout) {
            out->classData.drum.snare       = dud[0];
            out->classData.drum.tom2        = dud[1];
            out->classData.drum.tom1        = dud[2];
            out->classData.drum.floorTom    = dud[3];
            out->classData.drum.hihatCymbal = dud[4];
            out->classData.drum.rideCymbal  = dud[5];
            out->classData.drum.crashCymbal = dud[6];
        } else {
            out->classData.drum.snare       = ScaleVel7to8(dud[3]);
            out->classData.drum.tom1        = ScaleVel7to8(dud[2]);
            out->classData.drum.tom2        = ScaleVel7to8(dud[5]);
            out->classData.drum.floorTom    = ScaleVel7to8(dud[4]);
            out->classData.drum.hihatCymbal = ScaleVel7to8(dud[2]);
            out->classData.drum.rideCymbal  = ScaleVel7to8(dud[7]);
            out->classData.drum.crashCymbal = ScaleVel7to8(dud[7]);
        }
        return true;
    }
    if (dev_class == OPB::OrbisPadDeviceClass::Guitar) {
        out->classData.guitar.toneNumber = dud[0];
        out->classData.guitar.whammyBar  = dud[1];
        out->classData.guitar.tilt       = dud[2];
        out->classData.guitar.fret       = dud[3];
        out->classData.guitar.fretSolo   = dud[4];
        return true;
    }
    return false;
}

std::string GetActiveKitName(int slot) {
    if (slot < 1 || slot > kNumSlots) return {};
    const auto* k = g_slots[slot - 1].kit;
    return k ? k->name : std::string{};
}

std::size_t GetLoadedKitCount() {
    std::lock_guard<std::mutex> lk(g_kits_mu);
    return g_kits.size();
}

std::vector<XInputDeviceInfo> EnumerateXInputDevices() {
    std::vector<XInputDeviceInfo> out;
    if (SDL_InitSubSystem(SDL_INIT_GAMEPAD) == false) {
        // Already initialized counts as success — try anyway.
    }
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

bool ShouldHideFromUsbd(u16 vid, u16 pid) {
    bool any_enabled = false;
    for (int i = 0; i < kNumSlots; ++i) {
        if (Config::getSpecialPadLegacyPassUSBRawHID(i + 1)) {
            any_enabled = true;
            break;
        }
    }
    if (!any_enabled) return false;
    std::lock_guard<std::mutex> lk(g_kits_mu);
    for (const auto& k : g_kits) {
        if (k.vid == vid && k.pid == pid) return true;
    }
    return false;
}

}  // namespace Input::HidInstrument
