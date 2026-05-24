// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "input/hid_instrument.h"

#include <SDL3/SDL_hidapi.h>
#include <toml.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
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
    std::size_t report_length = 0;
    // dud_layout[i] = raw byte index for deviceUniqueData[i], or -1 for zero.
    std::array<int, kMaxDeviceUniqueData> dud_layout{-1, -1, -1, -1, -1, -1,
                                                     -1, -1, -1, -1, -1, -1};
    std::array<u32, 8> button_byte_0{};
    std::array<u32, 8> button_byte_1{};
    int hat_byte = -1;
    int tilt_byte = -1;
    int tilt_byte_high = -1;
    int tilt_baseline = 0x80;
    int tilt_scale = 90;
    int whammy_byte = -1;
    int touch_byte = -1;
    // Source byte for guitar pickup (Effects Switch), quantized to 0..4.
    int tone_byte = -1;
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
        k.report_length = static_cast<std::size_t>(toml::find_or<int>(root, "report_length", 0));
        k.hat_byte = toml::find_or<int>(root, "hat_byte", -1);
        k.tilt_byte = toml::find_or<int>(root, "tilt_byte", -1);
        k.tilt_byte_high = toml::find_or<int>(root, "tilt_byte_high", -1);
        k.tilt_baseline = toml::find_or<int>(root, "tilt_baseline", 0x80);
        k.tilt_scale = toml::find_or<int>(root, "tilt_scale", 90);
        k.whammy_byte = toml::find_or<int>(root, "whammy_byte", -1);
        k.touch_byte = toml::find_or<int>(root, "touch_byte", -1);
        k.tone_byte = toml::find_or<int>(root, "tone_byte", -1);
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

        auto load_button_table = [&](const char* section, std::array<u32, 8>& out) {
            if (!root.contains(section)) return;
            const auto& tbl = toml::find(root, section).as_table();
            for (const auto& [key, val] : tbl) {
                const u32 mask = ParseHexOrDec(key);
                const u32 btn = ButtonByName(val.as_string());
                for (int bit = 0; bit < 8; ++bit) {
                    if (mask & (1u << bit)) out[bit] |= btn;
                }
            }
        };
        load_button_table("buttons_byte_0", k.button_byte_0);
        load_button_table("buttons_byte_1", k.button_byte_1);

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
    SDL_hid_device* dev = nullptr;
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
        if (g_slots[i].dev && g_slots[i].device_path == path) {
            return true;
        }
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
                if (s.dev) {
                    LOG_INFO(Input, "HID instrument slot {}: flag disabled, closing device",
                             slot);
                    CloseSlot(s);
                }
                continue;
            }

            if (!s.dev) {
                std::vector<KitDef> snapshot;
                {
                    std::lock_guard<std::mutex> lk(g_kits_mu);
                    snapshot = g_kits;
                }
                // For each known kit VID:PID, enumerate every physical
                // instance on the USB bus and grab the first path that
                // isn't already claimed by another slot. This lets two
                // identical kits be opened simultaneously (e.g. two
                // GH5 guitars for co-op).
                for (const auto& kd : snapshot) {
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
                                 "HID instrument slot {}: opened {:04x}:{:04x} ({}) at {}",
                                 slot, kd.vid, kd.pid, kd.name, path);
                    }
                    SDL_hid_free_enumeration(head);
                    if (s.dev) break;
                }
                if (!s.dev && !s.open_failed_logged) {
                    LOG_WARNING(Input,
                                "HID instrument slot {}: no known kit found "
                                "(specialPadLegacyPassUSBRawHID{} is true)",
                                slot, slot);
                    s.open_failed_logged = true;
                }
                continue;
            }

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
    // Below baseline = pointed up; negate so positive out_x means tilted up.
    float x = -static_cast<float>(delta) / static_cast<float>(scale);
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
            out[1] = (w >= 0x80) ? static_cast<u8>((w - 0x80) * 2) : 0;
        }
        float acc_x = 0.0f, acc_y = 0.0f, acc_z = 0.0f;
        if (GetLatestAcceleration(slot, acc_x, acc_y, acc_z) && acc_x > 0.0f) {
            float t = acc_x * 255.0f;
            if (t > 255.0f) t = 255.0f;
            out[2] = static_cast<u8>(t);
        }
        u8 frets = at(0);
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
    if (raw_len > 0) {
        const u8 b0 = raw[0];
        for (int bit = 0; bit < 8; ++bit) {
            if (b0 & (1u << bit)) out |= kit->button_byte_0[bit];
        }
    }
    if (raw_len > 1) {
        const u8 b1 = raw[1];
        for (int bit = 0; bit < 8; ++bit) {
            if (b1 & (1u << bit)) out |= kit->button_byte_1[bit];
        }
    }
    if (kit->hat_byte >= 0 && static_cast<std::size_t>(kit->hat_byte) < raw_len) {
        switch (raw[kit->hat_byte]) {
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
