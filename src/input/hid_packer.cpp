// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Pure-logic packer for legacy raw-HID instruments. No SDL3 / threading
// dependency — the IO layer in hid_instrument.cpp owns those. Split out so
// the regression test harness (tests/hidtest) can link against this exact
// code without dragging the gamepad / hidraw stack into the test build.

#include "input/hid_kit_def.h"

#include <toml.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "common/config.h"
#include "common/logging/log.h"
#include "common/path_util.h"

namespace fs = std::filesystem;
namespace OPB = Libraries::Pad;

namespace Input::HidInstrument {

std::vector<KitDef> g_kits;
std::mutex g_kits_mu;
SlotState g_slots[kNumSlots];

namespace {

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

// Decode tilt → acceleration.x in [-1, 1] using the given raw buffer and
// kit parameters. The caller passes whichever frame they have; no slot
// state involved. This is what makes the packer testable in isolation —
// PackDeviceUniqueData passes its own `raw` arg instead of reaching into
// g_slots[slot].last_report.
bool ComputeAccelerationFrom(const KitDef& kit, const u8* raw, std::size_t raw_len,
                             float& out_x) {
    out_x = 0.0f;
    if (!raw || raw_len == 0) return false;
    if (kit.tilt_byte < 0 ||
        static_cast<std::size_t>(kit.tilt_byte) >= raw_len) {
        return false;
    }
    int rawv = raw[kit.tilt_byte];
    if (kit.tilt_byte_high >= 0 &&
        static_cast<std::size_t>(kit.tilt_byte_high) < raw_len) {
        rawv |= (raw[kit.tilt_byte_high] & 0x03) << 8;
    }
    const int delta = rawv - kit.tilt_baseline;
    const int scale = (kit.tilt_scale > 0) ? kit.tilt_scale : 128;
    float x = -static_cast<float>(delta) / static_cast<float>(scale);
    if (kit.tilt_invert) x = -x;
    if (x < -1.0f) x = -1.0f;
    if (x > 1.0f) x = 1.0f;
    out_x = x;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Kit loading
// ---------------------------------------------------------------------------

bool LoadKitFromToml(const std::string& file_path) {
    const fs::path file(file_path);
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
        k.solo_modifier_byte = toml::find_or<int>(root, "solo_modifier_byte", -1);
        k.solo_modifier_mask = static_cast<u8>(
            toml::find_or<int>(root, "solo_modifier_mask", 0));
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
                try { byte_idx = std::stoi(key.substr(13)); } catch (...) { continue; }
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

namespace {
std::once_flag g_kits_loaded_once;
}  // namespace

// Lazy-load all kit TOMLs exactly once, no SDL / no poll thread. The
// full EnsureInit() in the IO layer also chains through here, so the
// kit list is populated by whichever path hits first — sceUsbdGetDeviceList
// (which calls ShouldHideFromUsbd to decide whether to drop a device from
// the libusb list) typically fires during game boot, BEFORE any scePadRead
// triggers EnsureInit; without this hook the Les Paul + Riffmaster show up
// twice (once as the HID-passthrough pad, once via libusb).
void EnsureKitsLoaded() {
    std::call_once(g_kits_loaded_once, [] { LoadAllKits(); });
}

void LoadAllKits() {
    auto scan_dir = [](const fs::path& dir) {
        std::error_code ec;
        if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return;
        for (auto it = fs::recursive_directory_iterator(
                 dir, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec) && it->path().extension() == ".toml") {
                LoadKitFromToml(it->path().string());
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

// ---------------------------------------------------------------------------
// Slot-state accessors used by the rest of the emulator
// ---------------------------------------------------------------------------

bool GetLatestReport(int slot, u8* out, std::size_t* out_len) {
    if (slot < 1 || slot > kNumSlots) return false;
    if (!Config::getSpecialPadLegacyPassUSBRawHID(slot)) return false;
    EnsureInit();
    SlotState& s = g_slots[slot - 1];
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.has_data || s.last_report_len == 0) return false;
    if (out) std::memcpy(out, s.last_report, s.last_report_len);
    if (out_len) *out_len = s.last_report_len;
    return true;
}

bool GetLatestAcceleration(int slot, float& out_x, float& out_y, float& out_z) {
    out_x = out_y = out_z = 0.0f;
    if (slot < 1 || slot > kNumSlots) return false;
    if (!Config::getSpecialPadLegacyPassUSBRawHID(slot)) return false;
    EnsureInit();
    SlotState& s = g_slots[slot - 1];
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.has_data || s.last_report_len == 0 || !s.kit) return false;
    return ComputeAccelerationFrom(*s.kit, s.last_report, s.last_report_len, out_x);
}

// ---------------------------------------------------------------------------
// Packer
// ---------------------------------------------------------------------------

std::size_t PackDeviceUniqueData(int slot, const u8* raw, std::size_t raw_len,
                                 OPB::OrbisPadDeviceClass dev_class,
                                 u8 out[kMaxDeviceUniqueData]) {
    (void)dev_class;
    if (!raw || !out || raw_len == 0) return 0;
    const KitDef* kit = nullptr;
    if (slot >= 1 && slot <= 4) kit = g_slots[slot - 1].kit;
    if (!kit) {
        std::memset(out, 0, kMaxDeviceUniqueData);
        return kMaxDeviceUniqueData;
    }
    auto at = [&](int i) -> u8 {
        if (i < 0 || static_cast<std::size_t>(i) >= raw_len) return 0;
        return raw[i];
    };
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
    if (kit->guitar_ps4_layout) {
        std::memset(out, 0, kMaxDeviceUniqueData);
        if (kit->tone_byte >= 0) {
            const u8 tone_raw = at(kit->tone_byte);
            // PlasticBand spec: PS3 / Wii / X360 RB guitars report raw
            // 0x7F when the pickup switch is at rest — must be filtered
            // or the middle notch (vibe) constantly false-triggers and
            // collides with the real notch 3. PS4/PS5 don't use this
            // sentinel; on those, raw never lands exactly on 0x7F.
            // Five-notch quantization is `raw / (255 / 5)`, which is
            // identical for the PS3/Wii/X360 detent values (~25/76/121/
            // 178/229) AND the PS4 Mustang/Riffmaster discrete values
            // (~0x00/0x40/0x80/0xC0/0xFF).
            if (slot >= 1 && slot <= 4) {
                auto& last = g_slots[slot - 1].last_pickup_notch;
                if (tone_raw == 0x7F) {
                    out[0] = last;
                } else {
                    u8 notch = static_cast<u8>(tone_raw / (0xFFu / 5u));
                    if (notch > 4) notch = 4;
                    last = notch;
                    out[0] = notch;
                }
            } else {
                u8 notch = (tone_raw == 0x7F)
                               ? 0
                               : static_cast<u8>(tone_raw / (0xFFu / 5u));
                if (notch > 4) notch = 4;
                out[0] = notch;
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
        // Compute tilt from the raw frame we were given — *not* from slot
        // state. This lets tests pass arbitrary frames in without first
        // populating g_slots[slot].last_report.
        float acc_x = 0.0f;
        if (ComputeAccelerationFrom(*kit, raw, raw_len, acc_x) && acc_x > 0.0f) {
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
                if (frets & (1u << b)) remapped |= (1u << kit->dud0_bit_remap[b]);
            }
            frets = remapped;
        }
        // Three solo-fret encodings, in priority order:
        //   1. solo_modifier_byte/mask set (X360 RB Guitar, Strat-style):
        //      while the modifier bit is held, the main fret bits are
        //      treated as a SOLO press — routed to dud[4], dud[3] = 0.
        //   2. solo_fret_byte set (PS4 Mustang, PS5 Riffmaster):
        //      a dedicated byte carries the solo bitmask independently
        //      from the main fret byte. Both can fire simultaneously.
        //   3. Neither: dud[3] = main frets, dud[4] = 0.
        const bool solo_active =
            kit->solo_modifier_byte >= 0 &&
            kit->solo_modifier_mask != 0 &&
            static_cast<std::size_t>(kit->solo_modifier_byte) < raw_len &&
            (raw[kit->solo_modifier_byte] & kit->solo_modifier_mask) != 0;
        if (solo_active) {
            out[3] = 0;
            out[4] = frets;
        } else {
            out[3] = frets;
            if (kit->solo_fret_byte >= 0 &&
                static_cast<std::size_t>(kit->solo_fret_byte) < raw_len) {
                out[4] = at(kit->solo_fret_byte);
            }
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
            if (flags & (1u << b)) remapped |= (1u << kit->dud0_bit_remap[b]);
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
    if (!raw || raw_len == 0) return 0;
    const KitDef* kit = nullptr;
    if (slot >= 1 && slot <= 4) kit = g_slots[slot - 1].kit;
    if (!kit) return 0;
    // PackDeviceUniqueData clears dud[0] (the fret slot) whenever a menu
    // bit is held in raw[1] — so the game's fret state goes to 0 while
    // Start/Select is down. But PackButtons reads the same byte the frets
    // live in and fires their face-button bits too, which means in-game
    // a Start+green chord goes through as Cross|Options simultaneously
    // (the "Santroller HID freak-out" report). Mask the fret bits on
    // the fret_byte when the suppression is active, so PackButtons agrees
    // with PackDeviceUniqueData about which inputs are live.
    const bool suppress_frets =
        kit->clear_dud0_when_raw1_bits && raw_len > 1 &&
        (raw[1] & kit->clear_dud0_when_raw1_bits);
    u32 out = 0;
    for (const auto& [byte_idx, table] : kit->button_bytes) {
        if (byte_idx < 0 || static_cast<std::size_t>(byte_idx) >= raw_len) continue;
        u8 b = raw[byte_idx];
        if (suppress_frets && byte_idx == kit->fret_byte) {
            b &= ~kit->fret_mask;
        }
        for (int bit = 0; bit < 8; ++bit) {
            if (b & (1u << bit)) out |= table[bit];
        }
    }
    if (kit->hat_byte >= 0 && static_cast<std::size_t>(kit->hat_byte) < raw_len) {
        const u8 hat = raw[kit->hat_byte];
        if (kit->source == "xinput") {
            // FillXInputReport writes byte 2 as a 4-bit bitmap (up=bit0,
            // down=bit1, left=bit2, right=bit3) — not as an HID HAT
            // position. Decode bit-for-bit so XInput strum doesn't read
            // out as diagonals.
            if (hat & 0x01) out |= static_cast<u32>(B::Up);
            if (hat & 0x02) out |= static_cast<u32>(B::Down);
            if (hat & 0x04) out |= static_cast<u32>(B::Left);
            if (hat & 0x08) out |= static_cast<u32>(B::Right);
        } else {
            switch (hat & 0x0F) {
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
    }
    return out;
}

bool ParseTypedData(int slot, const u8* dud, std::size_t dud_len,
                    OPB::OrbisPadDeviceClass dev_class,
                    OPB::OrbisPadDeviceClassData* out) {
    if (!dud || !out || dud_len < 8) return false;
    std::memset(out, 0, sizeof(*out));
    out->deviceClass = dev_class;
    out->bDataValid = true;
    if (dev_class == OPB::OrbisPadDeviceClass::Drum) {
        // PS4 RB 4-Lane wire byte order (PlasticBand spec):
        //   dud[0] red/snare, dud[1] blue, dud[2] yellow, dud[3] green,
        //   dud[4] yellow-cym, dud[5] blue-cym, dud[6] green-cym.
        // OrbisPadDeviceClassData::drum struct field order mirrors this:
        //   snare, tom1, tom2, floorTom, hihatCymbal, rideCymbal, crashCymbal.
        // So tom1 = blue (dud[1]) and tom2 = yellow (dud[2]). The previous
        // code had them swapped, which surfaced as reversed blue/yellow
        // pads through the typed-data API. The else-branch also handles
        // generic dud_layout[]-driven kits, where dud_layout[i] is meant to
        // align with the same per-index semantics — no special remapping.
        const KitDef* kit = (slot >= 1 && slot <= kNumSlots) ? g_slots[slot - 1].kit
                                                              : nullptr;
        const bool ps4 = kit && kit->drum_ps4_layout;
        auto scale = [&](u8 v) { return ps4 ? v : ScaleVel7to8(v); };
        out->classData.drum.snare       = scale(dud[0]);
        out->classData.drum.tom1        = scale(dud[1]);
        out->classData.drum.tom2        = scale(dud[2]);
        out->classData.drum.floorTom    = scale(dud[3]);
        out->classData.drum.hihatCymbal = scale(dud[4]);
        out->classData.drum.rideCymbal  = scale(dud[5]);
        out->classData.drum.crashCymbal = scale(dud[6]);
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

std::string GetActiveKitSource(int slot) {
    if (slot < 1 || slot > kNumSlots) return {};
    const auto* k = g_slots[slot - 1].kit;
    return k ? k->source : std::string{};
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
    // sceUsbdGetDeviceList usually runs during game boot, before any
    // scePadRead triggers EnsureInit(). Populate the kit list lazily here
    // so the very first libusb enumeration sees a non-empty g_kits.
    EnsureKitsLoaded();
    std::lock_guard<std::mutex> lk(g_kits_mu);
    for (const auto& k : g_kits) {
        if (k.vid == vid && k.pid == pid) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Test helpers — used by tests/hidtest. ResetForTesting only zeroes the
// data fields; SDL handles (if any) are released by hid_instrument.cpp's
// IO path. In a test context there are no handles to release.
// ---------------------------------------------------------------------------

namespace Testing {

void ResetForTesting() {
    for (auto& s : g_slots) {
        s.dev = nullptr;
        s.gamepad = nullptr;
        s.vid = s.pid = 0;
        s.device_path.clear();
        s.kit = nullptr;
        std::lock_guard<std::mutex> lk(s.mu);
        s.has_data = false;
        s.last_report_len = 0;
        s.open_failed_logged = false;
    }
    std::lock_guard<std::mutex> lk(g_kits_mu);
    g_kits.clear();
}

bool BindKitFromTomlForTesting(int slot, const std::string& toml_path) {
    if (slot < 1 || slot > kNumSlots) return false;
    if (!LoadKitFromToml(toml_path)) return false;
    std::lock_guard<std::mutex> lk(g_kits_mu);
    for (auto& k : g_kits) {
        if (k.source_file == toml_path) {
            g_slots[slot - 1].kit = &k;
            return true;
        }
    }
    return false;
}

}  // namespace Testing

}  // namespace Input::HidInstrument
