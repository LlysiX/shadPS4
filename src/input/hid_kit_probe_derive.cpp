// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "input/hid_kit_probe_data.h"

#include <algorithm>
#include <array>
#include <map>
#include <sstream>
#include <tuple>
#include <utility>
#include <vector>

namespace Input::HidInstrument {

namespace {
bool IsGuitarType(ProbeDeviceType t) {
    return t == ProbeDeviceType::Guitar || t == ProbeDeviceType::GuitarSolo;
}
}  // namespace

std::string DeriveKitToml(const KitProbeData& data) {
    const int report_len = data.report_length;
    const auto& results = data.results;
    const auto& baseline_max = data.baseline_max;
    const auto& baseline_min = data.baseline_min;
    const auto& motion_bytes = data.motion_bytes;

    auto velByte = [&](const std::string& key) -> int {
        for (const auto& r : results) {
            if (r.key != key || !r.captured) continue;
            int best = -1, best_range = 0;
            for (int i = 3; i < report_len; ++i) {
                if (motion_bytes.count(i)) continue;
                const auto& b = r.bytes[i];
                if (b.samples == 0) continue;
                if (b.max < 0x20) continue;
                const int baseline_floor = baseline_min[i];
                const int baseline_ceil = baseline_max[i];
                const bool quiet_at_idle =
                    (baseline_ceil <= 4) ||
                    (baseline_floor >= 0x7C && baseline_ceil <= 0x84);
                if (!quiet_at_idle) continue;
                const int range = b.max - b.min;
                if (range > best_range && i != 26) {
                    best_range = range;
                    best = i;
                }
            }
            return best;
        }
        return -1;
    };
    auto flagBit = [&](const std::string& key, int rawByte) -> uint8_t {
        for (const auto& r : results) {
            if (r.key != key || !r.captured) continue;
            const auto& b = r.bytes[rawByte];
            if (b.samples == 0) return 0;
            return static_cast<uint8_t>(b.max);
        }
        return 0;
    };
    // Collect every byte index that gained exactly one set bit during the
    // step. PS4 RB and PS5 Riffmaster expose each fret on two bytes (face
    // flag byte AND dedicated bitmap byte) so the caller usually inspects
    // all candidates before picking a winner.
    auto detectAllFlagCandidates =
        [&](const std::string& key) -> std::vector<std::pair<int, uint8_t>> {
        std::vector<std::pair<int, uint8_t>> out;
        for (const auto& r : results) {
            if (r.key != key || !r.captured) continue;
            for (int i = 0; i < report_len; ++i) {
                if (motion_bytes.count(i)) continue;
                const auto& b = r.bytes[i];
                if (b.samples == 0) continue;
                const int baseline = baseline_max[i];
                if (b.max <= baseline) continue;
                const int diff = b.max & ~baseline;
                if (diff == 0) continue;
                if ((diff & (diff - 1)) != 0) continue;
                out.push_back({i, static_cast<uint8_t>(diff)});
            }
            break;
        }
        return out;
    };
    // Some SDL gamepad mappings co-fire two buttons when the user only
    // pressed one — the Xbox 360 GH guitar and the CRKD in PC mode 8
    // both report byte 1 = 0x11 (Start | R3) on a single Start press. The
    // strict single-bit candidate search above rejects 0x11 entirely; this
    // multi-bit fallback isolates the lowest set bit so we at least
    // capture Start as bit 0. The higher bit (R3) stays unmapped, which
    // is what you want — guitars don't use stick clicks.
    auto detectMultiBitFallback =
        [&](const std::string& key) -> std::vector<std::pair<int, uint8_t>> {
        std::vector<std::pair<int, uint8_t>> out;
        for (const auto& r : results) {
            if (r.key != key || !r.captured) continue;
            for (int i = 0; i < report_len; ++i) {
                if (motion_bytes.count(i)) continue;
                const auto& b = r.bytes[i];
                if (b.samples == 0) continue;
                const int baseline = baseline_max[i];
                if (b.max <= baseline) continue;
                const int diff = b.max & ~baseline;
                if (diff == 0) continue;
                // Lowest set bit only.
                out.push_back({i, static_cast<uint8_t>(diff & -diff)});
            }
            break;
        }
        return out;
    };
    // Single-button steps (Start, Select, etc.) pick the cleanest candidate:
    // the source byte with the lowest baseline value. Avoids latching onto a
    // byte that already has unrelated flags set (e.g. HAT low nibble).
    auto detectFlagByteAndBit =
        [&](const std::string& key) -> std::pair<int, uint8_t> {
        auto cands = detectAllFlagCandidates(key);
        if (cands.empty()) cands = detectMultiBitFallback(key);
        if (cands.empty()) return {-1, 0};
        auto best = cands.front();
        int best_baseline = baseline_max[best.first];
        for (std::size_t k = 1; k < cands.size(); ++k) {
            const int base = baseline_max[cands[k].first];
            if (base < best_baseline) {
                best = cands[k];
                best_baseline = base;
            }
        }
        return best;
    };
    auto detectHatByte = [&]() -> int {
        static const std::vector<std::string> keys = {
            "dpad_up", "dpad_down", "dpad_left", "dpad_right",
            "strum_up", "strum_down"};
        for (const auto& r : results) {
            if (!r.captured) continue;
            if (std::find(keys.begin(), keys.end(), r.key) == keys.end()) continue;
            for (int i = 0; i < report_len; ++i) {
                if (motion_bytes.count(i)) continue;
                const auto& b = r.bytes[i];
                if (b.samples == 0) continue;
                if (b.min <= 7 && b.max <= 0x0F && (b.max - b.min) > 0) {
                    return i;
                }
            }
        }
        return 2;
    };
    const int hatByte = detectHatByte();
    auto [selByte, b_sel] = detectFlagByteAndBit("button_select");
    auto [staByte, b_sta] = detectFlagByteAndBit("button_start");
    if (selByte < 0) selByte = 1;
    if (staByte < 0) staByte = 1;

    struct DudEntry { int idx; int rawByte; const char* comment; };
    std::vector<DudEntry> dudPlan;
    struct ButtonBit { int byte; uint8_t mask; const char* name; const char* origin; };
    std::vector<ButtonBit> button_bits;
    struct ScaleEntry { int dudIdx; const char* stepKey; const char* comment; };
    std::vector<ScaleEntry> scalePlan;
    const char* deviceClass = "drum";

    if (IsGuitarType(data.device_type)) {
        deviceClass = "guitar";
        dudPlan = {
            {2, velByte("whammy_bar"), "whammy bar"},
            {3, velByte("touch_slider"), "touch slider"},
        };
        struct FretMap { const char* step; const char* name; const char* origin; };
        const FretMap fretMaps[] = {
            {"green_fret", "cross", "green fret"},
            {"red_fret", "circle", "red fret"},
            {"yellow_fret", "triangle", "yellow fret"},
            {"blue_fret", "square", "blue fret"},
            {"orange_fret", "l1", "orange fret"},
        };
        std::array<std::vector<std::pair<int, uint8_t>>, 5> fretCandidates;
        std::map<int, int> coverage;
        for (int i = 0; i < 5; ++i) {
            fretCandidates[i] = detectAllFlagCandidates(fretMaps[i].step);
            for (const auto& [byte, mask] : fretCandidates[i]) {
                ++coverage[byte];
            }
        }
        auto comboBitsForByte = [&](int byte) -> std::pair<uint8_t, uint8_t> {
            uint8_t g = 0, b = 0;
            for (const auto& [bb, mm] : fretCandidates[0])
                if (bb == byte) { g = mm; break; }
            for (const auto& [bb, mm] : fretCandidates[3])
                if (bb == byte) { b = mm; break; }
            return {g, b};
        };
        auto byteValidatesCombo = [&](int byte) -> bool {
            auto [g, b] = comboBitsForByte(byte);
            if (g == 0 || b == 0) return false;
            for (const auto& r : results) {
                if (r.key != "green_blue" || !r.captured) continue;
                if (byte < 0 || byte >= report_len) return false;
                const int max = r.bytes[byte].max;
                const int baseline = baseline_max[byte];
                return (max & ~baseline) == (g | b);
            }
            return false;
        };
        int chosenByte = -1, chosenCoverage = 0, chosenBaseline = 0x7FFFFFFF;
        bool chosenValidated = false;
        for (const auto& [byte, count] : coverage) {
            const int base = baseline_max[byte];
            const bool validated = byteValidatesCombo(byte);
            const auto rank = [&](bool v, int c, int b) {
                return std::make_tuple(v ? 1 : 0, c, -b);
            };
            if (rank(validated, count, base) >
                rank(chosenValidated, chosenCoverage, chosenBaseline)) {
                chosenByte = byte;
                chosenCoverage = count;
                chosenBaseline = base;
                chosenValidated = validated;
            }
        }
        if (chosenByte >= 0) {
            for (int i = 0; i < 5; ++i) {
                for (const auto& [byte, mask] : fretCandidates[i]) {
                    if (byte != chosenByte) continue;
                    button_bits.push_back({byte, mask, fretMaps[i].name,
                                           fretMaps[i].origin});
                    break;
                }
            }
        }
        scalePlan = {};
    } else {
        deviceClass = "drum";
        // 5-lane GH kits expose orange as a PAD (lower lane); RB kits expose
        // it as a cymbal. Either step can fill the orange velocity slot.
        const int orange_byte = (velByte("orange_pad") >= 0)
                                    ? velByte("orange_pad")
                                    : velByte("orange_cymbal");
        const char* orange_step = (velByte("orange_pad") >= 0)
                                      ? "orange_pad"
                                      : "orange_cymbal";
        dudPlan = {
            {2, velByte("yellow_cymbal"), "yellow velocity"},
            {3, velByte("red_pad"), "red velocity"},
            {4, velByte("green_pad"), "green velocity"},
            {5, velByte("blue_pad"), "blue velocity"},
            {6, velByte("kick_pedal"), "kick velocity"},
            {7, orange_byte, "orange velocity"},
        };
        // Face buttons can live on different bytes per kit (PS3 RB drums put
        // them on byte 1; Santroller's drum profile puts them at the end of
        // the report). Use detectFlagByteAndBit so each button picks its own
        // source byte instead of assuming byte 0.
        auto [sqByte, b_sq] = detectFlagByteAndBit("button_square");
        auto [crByte, b_cr] = detectFlagByteAndBit("button_cross");
        auto [ciByte, b_ci] = detectFlagByteAndBit("button_circle");
        auto [trByte, b_tr] = detectFlagByteAndBit("button_triangle");
        auto [kickByte, b_kick_raw] = detectFlagByteAndBit("kick_pedal");
        auto [orByte, b_or_raw] = detectFlagByteAndBit(orange_step);
        // Strip face-button bits that also lit up the kick/orange step, so we
        // don't double-map the same bit (PS3 RB drums fire face button +
        // pad flag together when you hit a pad).
        const uint8_t face_on_kick = (kickByte == sqByte ? b_sq : 0) |
                                      (kickByte == crByte ? b_cr : 0) |
                                      (kickByte == ciByte ? b_ci : 0) |
                                      (kickByte == trByte ? b_tr : 0);
        const uint8_t face_on_orange = (orByte == sqByte ? b_sq : 0) |
                                        (orByte == crByte ? b_cr : 0) |
                                        (orByte == ciByte ? b_ci : 0) |
                                        (orByte == trByte ? b_tr : 0);
        const uint8_t b_kick = b_kick_raw & ~face_on_kick;
        const uint8_t b_orange = b_or_raw & ~face_on_orange;
        button_bits = {
            {sqByte, b_sq, "square", "blue pad"},
            {crByte, b_cr, "cross", "green pad"},
            {ciByte, b_ci, "circle", "red pad"},
            {trByte, b_tr, "triangle", "yellow pad / yellow cymbal"},
            {kickByte, b_kick, "l1", "kick pedal"},
            {orByte, b_orange, "r1", "orange pad/cymbal (5th lane in GH-mode)"},
        };
        scalePlan = {
            {2, "yellow_cymbal", "yellow"},
            {3, "red_pad", "red"},
            {4, "green_pad", "green"},
            {5, "blue_pad", "blue"},
            {6, "kick_pedal", "kick"},
            {7, orange_step, "orange"},
        };
    }

    int dud[12];
    dud[0] = 0;
    dud[1] = 1;
    for (int i = 2; i <= 7; ++i) dud[i] = -1;
    for (const auto& e : dudPlan) dud[e.idx] = e.rawByte;
    dud[8] = hatByte;
    dud[9] = -1;
    dud[10] = -1;
    dud[11] = std::max(0, report_len - 1);

    std::ostringstream os;
    os << "# Generated by KitProbeDialog. Picked up at next launch.\n\n";
    os << "schema       = \"shadps4-legacy-instrument/v1\"\n";
    os << "vendor_id    = \"0x" << std::hex << data.vid << "\"\n";
    os << "product_id   = \"0x" << data.pid << "\"\n";
    os << std::dec;
    os << "name         = \"" << data.device_name << "\"\n";
    os << "device_class = \"" << deviceClass << "\"\n";
    if (data.is_xinput) os << "source       = \"xinput\"\n";
    os << "report_length = " << report_len << "\n";
    // The runtime ignores `device_unique_data` when `guitar_ps4_layout` or
    // `drum_ps4_layout` is set — the wire-format byte order is hardcoded
    // there. Emitting it for those kits just confuses hand-editing.
    const bool ps4_layout = (IsGuitarType(data.device_type)) ||
                            (data.device_type == ProbeDeviceType::ProDrum);
    if (!ps4_layout) {
        os << "device_unique_data = [";
        for (int i = 0; i < 12; ++i) {
            if (i) os << ", ";
            os << dud[i];
        }
        os << "]\n";
    }
    if (selByte == 1 && staByte == 1) {
        os << "clear_dud0_when_raw1_bits = 0x" << std::hex
           << int(b_sel | b_sta) << std::dec << "\n";
    }

    // Detect which raw byte holds the fret bitmap and whether the bit order
    // needs remapping to PS4-native order in dud[0].
    int fretByte = -1;
    int remap[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    bool needs_remap = false;
    if (IsGuitarType(data.device_type)) {
        struct FretMap { const char* step; int ps4_bit; };
        const FretMap frets[] = {
            {"green_fret", 0}, {"red_fret", 1}, {"yellow_fret", 2},
            {"blue_fret", 3}, {"orange_fret", 4},
        };
        uint8_t fretMaskBits = 0;
        std::map<int, int> cov;
        std::array<std::vector<std::pair<int, uint8_t>>, 5> cands;
        for (int i = 0; i < 5; ++i) {
            cands[i] = detectAllFlagCandidates(frets[i].step);
            for (const auto& [byte, _] : cands[i]) ++cov[byte];
        }
        int chosen = -1, chosenCov = 0, chosenBase = 0x7FFFFFFF;
        for (const auto& [byte, count] : cov) {
            const int base = baseline_max[byte];
            if (count > chosenCov || (count == chosenCov && base < chosenBase)) {
                chosen = byte;
                chosenCov = count;
                chosenBase = base;
            }
        }
        for (int i = 0; i < 5; ++i) {
            for (const auto& [byte, mask] : cands[i]) {
                if (byte != chosen) continue;
                if (fretByte < 0) fretByte = byte;
                fretMaskBits |= mask;
                for (int b = 0; b < 8; ++b) {
                    if (mask & (1 << b)) {
                        if (b != frets[i].ps4_bit) needs_remap = true;
                        remap[b] = frets[i].ps4_bit;
                        break;
                    }
                }
                break;
            }
        }
        if (needs_remap) {
            os << "dud0_bit_remap = [";
            for (int i = 0; i < 8; ++i) {
                if (i) os << ", ";
                os << remap[i];
            }
            os << "]\n";
        }
        if (fretMaskBits != 0 && fretMaskBits != 0xFF) {
            os << "fret_mask = 0x" << std::hex << int(fretMaskBits)
               << std::dec << "\n";
        }
    }
    os << "hat_byte = " << hatByte << "\n";
    if (data.device_type == ProbeDeviceType::ProDrum) {
        os << "drum_ps4_layout = true\n";
        const int red_b = velByte("red_pad");
        const int blue_b = velByte("blue_pad");
        const int yellow_b = velByte("yellow_pad");
        const int green_b = velByte("green_pad");
        const int y_cym = velByte("yellow_cymbal");
        const int b_cym = velByte("blue_cymbal");
        const int g_cym = velByte("green_cymbal");
        const int o_cym = velByte("orange_cymbal");
        os << "drum_red_byte           = " << red_b << "\n";
        os << "drum_blue_byte          = " << blue_b << "\n";
        os << "drum_yellow_byte        = "
           << (yellow_b >= 0 ? yellow_b : y_cym) << "\n";
        os << "drum_green_byte         = " << green_b << "\n";
        os << "drum_yellow_cymbal_byte = "
           << (y_cym >= 0 ? y_cym : yellow_b) << "\n";
        os << "drum_blue_cymbal_byte   = " << b_cym << "\n";
        os << "drum_green_cymbal_byte  = "
           << (g_cym >= 0 ? g_cym : o_cym) << "\n";
    }
    if (IsGuitarType(data.device_type)) {
        os << "guitar_ps4_layout = true\n";
        if (fretByte >= 0) os << "fret_byte = " << fretByte << "\n";
        const int whammy = velByte("whammy_bar");
        const int touch = velByte("touch_slider");
        const int fx = velByte("fx_switch");
        if (whammy >= 0) {
            os << "whammy_byte = " << whammy << "\n";
            const int wb = baseline_max[whammy];
            if (wb < 0x40) os << "whammy_baseline = 0\n";
        }
        // PS3 GH/RB guitars have a continuous touch strip (tone slider).
        // PS4 RB Mustang / PS5 Riffmaster / X360 RB guitars have a
        // discrete pickup/FX switch instead. Both map to dud[0] in PS4
        // wire format via tone_byte, so the runtime treats them the
        // same; we just prefer fx_switch when present.
        // Pick the source byte for dud[0] (tone position). PS3 GH/RB
        // guitars feed it from a continuous touch strip; PS4/PS5/X360
        // guitars feed it from a discrete pickup/FX switch — the runtime
        // (PackDeviceUniqueData) treats both as a 0x10–0xFF ramp mapped
        // onto positions 1–4. Prefer the FX switch when both are present
        // (a Mustang user might have noisily brushed the touch_slider
        // step even though the kit has no strip).
        const int tone_src = (fx >= 0) ? fx : touch;
        if (touch >= 0) os << "touch_byte  = " << touch << "\n";
        if (tone_src >= 0) os << "tone_byte   = " << tone_src << "\n";
        // PS4/PS5 RB guitars have a second set of solo frets on the upper
        // neck — they pack into dud[4] (fretSolo). Pick the byte with max
        // coverage across all 5 solo-fret presses, same algorithm as for
        // the main fret_byte but limited to the upper-neck capture steps.
        struct SoloMap { const char* step; const char* name; const char* origin; };
        const SoloMap solo_frets[] = {
            {"solo_green_fret",  "cross",    "solo green fret"},
            {"solo_red_fret",    "circle",   "solo red fret"},
            {"solo_yellow_fret", "triangle", "solo yellow fret"},
            {"solo_blue_fret",   "square",   "solo blue fret"},
            {"solo_orange_fret", "l1",       "solo orange fret"},
        };
        std::array<std::vector<std::pair<int, uint8_t>>, 5> solo_cands;
        std::map<int, int> solo_cov;
        for (int i = 0; i < 5; ++i) {
            solo_cands[i] = detectAllFlagCandidates(solo_frets[i].step);
            for (const auto& [byte, _] : solo_cands[i]) ++solo_cov[byte];
        }
        int solo_byte = -1, solo_cov_best = 0, solo_base_best = 0x7FFFFFFF;
        for (const auto& [byte, count] : solo_cov) {
            const int base = baseline_max[byte];
            if (count > solo_cov_best ||
                (count == solo_cov_best && base < solo_base_best)) {
                solo_byte = byte;
                solo_cov_best = count;
                solo_base_best = base;
            }
        }
        if (solo_byte >= 0 && solo_byte != fretByte) {
            os << "solo_fret_byte = " << solo_byte << "\n";
            // ALSO map each solo fret bit to its PS4 face button. Without
            // this, pressing a solo fret only sets dud[4] (the fretSolo
            // flag byte) — RB4 sees no button press, so the note isn't
            // recognised as held. Real PS4 Mustang / PS5 Riffmaster fire
            // the face button alongside the solo bit; the wizard
            // synthesises the same behaviour by emitting
            // [buttons_byte_<solo>] with the same bit→name table the main
            // fret byte got.
            for (int i = 0; i < 5; ++i) {
                for (const auto& [byte, mask] : solo_cands[i]) {
                    if (byte != solo_byte) continue;
                    button_bits.push_back({byte, mask, solo_frets[i].name,
                                           solo_frets[i].origin});
                    break;
                }
            }
        } else if (solo_byte < 0 || solo_byte == fretByte) {
            // X360 RB Guitar / Strat-style: solo frets land on the SAME
            // byte as main frets (their bits coincide), with a separate
            // modifier button (L3 on X360, R3 on some Strats) held down to
            // distinguish a solo press from a regular fret. Look for a
            // (byte, bit) pair that appears in EVERY solo step's candidate
            // list and is distinct from fretByte — that's the modifier.
            std::map<std::pair<int, uint8_t>, int> mod_count;
            for (int i = 0; i < 5; ++i) {
                for (const auto& [byte, mask] : solo_cands[i]) {
                    if (byte == fretByte) continue;
                    ++mod_count[{byte, mask}];
                }
            }
            int mod_byte = -1; uint8_t mod_mask = 0;
            for (const auto& [bm, count] : mod_count) {
                // Require ALL 5 solo presses to have surfaced this same
                // (byte, bit) — that's the modifier signature.
                if (count == 5) {
                    mod_byte = bm.first;
                    mod_mask = bm.second;
                    break;
                }
            }
            if (mod_byte >= 0) {
                os << "solo_modifier_byte = " << mod_byte << "\n";
                os << "solo_modifier_mask = 0x" << std::hex
                   << int(mod_mask) << std::dec << "\n";
            }
        }
    }
    if (IsGuitarType(data.device_type) && !motion_bytes.empty()) {
        // Pick the motion byte whose tilt_up range pushes furthest OUTSIDE
        // its idle baseline range — that's the tilt sensor. The old code
        // picked *motion_bytes.begin() (lowest index) which, on the PS5
        // Riffmaster, is byte 12 (a noisy sequence-ish counter that already
        // sweeps 0..255 during idle); the real tilt flag is byte 42 but
        // it was passed over. Same problem hit the CRKD in PC mode 8 where
        // the wizard's first-motion-byte pick required a manual remap to
        // get tilt working in-game.
        auto tiltUpStats = [&](int byte_idx) -> std::pair<int, int> {
            for (const auto& r : results) {
                if (r.key != "tilt_up" || !r.captured) continue;
                if (byte_idx < 0 || byte_idx >= report_len) break;
                return {r.bytes[byte_idx].min, r.bytes[byte_idx].max};
            }
            return {-1, -1};
        };
        int tilt = -1;
        int tilt_shift = -1;
        int tilt_up_min = 0, tilt_up_max = 0;
        for (int candidate : motion_bytes) {
            if (candidate < 0 || candidate >= report_len) continue;
            auto [up_min, up_max] = tiltUpStats(candidate);
            if (up_min < 0) continue;
            const int excess_high = std::max(0, up_max - baseline_max[candidate]);
            const int excess_low = std::max(0, baseline_min[candidate] - up_min);
            const int shift = excess_high + excess_low;
            if (shift > tilt_shift) {
                tilt_shift = shift;
                tilt = candidate;
                tilt_up_min = up_min;
                tilt_up_max = up_max;
            }
        }
        // Require a meaningful shift (≥ 32 raw units) before claiming a
        // tilt sensor exists. Many XInput synth captures and counter-only
        // devices have NO real tilt and would otherwise get a phantom
        // tilt_byte that constantly fires in-game.
        if (tilt >= 0 && tilt_shift >= 32) {
            os << "tilt_byte      = " << tilt << "\n";
            const bool has_high = motion_bytes.count(tilt + 1) > 0;
            const int baseline = (baseline_min[tilt] + baseline_max[tilt]) / 2;
            const int up_delta_high = tilt_up_max - baseline;
            const int up_delta_low = baseline - tilt_up_min;
            const bool invert = up_delta_high > up_delta_low;
            if (has_high) {
                os << "tilt_byte_high = " << (tilt + 1) << "\n";
                os << "tilt_baseline  = 512\n";
                os << "tilt_scale     = 128\n";
            } else {
                os << "tilt_baseline  = " << baseline << "\n";
                os << "tilt_scale     = 80\n";
            }
            os << "tilt_invert    = " << (invert ? "true" : "false") << "\n";
        }
    }
    if (!motion_bytes.empty()) {
        os << "motion_bytes = [";
        bool first = true;
        for (int b : motion_bytes) {
            if (!first) os << ", ";
            os << b;
            first = false;
        }
        os << "]\n";
    }

    // Fold Start/Select into the per-byte map so each TOML section gets
    // emitted exactly once.
    const char* selName =
        (IsGuitarType(data.device_type)) ? "left" : "touchpad";
    const char* selOrigin = (IsGuitarType(data.device_type))
                                ? "Select (Star Power)"
                                : "Select";
    if (b_sel) button_bits.push_back({selByte, b_sel, selName, selOrigin});
    if (b_sta) button_bits.push_back({staByte, b_sta, "options", "Start"});

    std::map<int, std::vector<ButtonBit>> by_byte;
    for (const auto& b : button_bits) {
        if (b.mask != 0) by_byte[b.byte].push_back(b);
    }
    auto emit_bit = [&](uint8_t bit, const char* name, const char* origin) {
        os << "\"0x" << std::hex;
        if (bit < 0x10) os << "0";
        os << int(bit) << std::dec << "\" = \"" << name << "\"";
        if (origin && *origin) os << "  # " << origin;
        os << '\n';
    };
    for (const auto& [byte_idx, bits] : by_byte) {
        os << "\n[buttons_byte_" << byte_idx << "]\n";
        std::map<uint8_t, std::pair<const char*, const char*>> uniq;
        for (const auto& b : bits) {
            uniq.try_emplace(b.mask, std::make_pair(b.name, b.origin));
        }
        for (const auto& [mask, name_origin] : uniq) {
            emit_bit(mask, name_origin.first, name_origin.second);
        }
    }

    bool emitted_scaling_header = false;
    for (const auto& s : scalePlan) {
        int bestByte = -1, bestRange = 0;
        for (const auto& r : results) {
            if (r.key != s.stepKey || !r.captured) continue;
            for (int i = 3; i < report_len; ++i) {
                if (motion_bytes.count(i)) continue;
                const auto& b = r.bytes[i];
                if (b.samples == 0) continue;
                const int range = b.max - b.min;
                if (range > bestRange && b.max >= 0x10 && i != 26) {
                    bestRange = range;
                    bestByte = i;
                }
            }
        }
        if (bestByte < 0) continue;
        int lo = -1, hi = -1;
        for (const auto& r : results) {
            if (r.key != s.stepKey || !r.captured) continue;
            lo = std::max(0, baseline_max[bestByte]) + 2;
            hi = r.bytes[bestByte].max;
            break;
        }
        if (hi <= lo) continue;
        if (!emitted_scaling_header) {
            os << "\n[velocity_scaling]\n";
            emitted_scaling_header = true;
        }
        os << '"' << s.dudIdx << "\" = { lo = " << lo << ", hi = " << hi
           << " }  # " << s.comment << '\n';
    }

    return os.str();
}

void DeriveBaselineAndMotion(KitProbeData& data) {
    std::array<int, 64> base_max{};
    std::array<int, 64> base_min{};
    base_min.fill(0xFF);
    data.motion_bytes.clear();

    bool has_idle_baseline = false;
    bool has_any_baseline = false;

    // First pass: extract baseline
    for (const auto& r : data.results) {
        if (r.key == "_idle_baseline" && r.captured && !r.bytes.empty()) {
            has_idle_baseline = true;
            has_any_baseline = true;
            for (int i = 0; i < data.report_length; ++i) {
                base_max[i] = r.bytes[i].max;
                base_min[i] = r.bytes[i].min;
            }
        }
    }

    // Fallback for v < 4: extract baseline from _motion_baseline if no _idle_baseline found
    if (!has_idle_baseline) {
        for (const auto& r : data.results) {
            if (r.key == "_motion_baseline" && r.captured && !r.bytes.empty()) {
                has_any_baseline = true;
                for (int i = 0; i < data.report_length; ++i) {
                    base_max[i] = std::max<int>(base_max[i], r.bytes[i].max);
                    base_min[i] = std::min<int>(base_min[i], r.bytes[i].min);
                }
            }
        }
    }

    if (has_any_baseline) {
        for (int i = 0; i < 64; ++i) {
            data.baseline_max[i] = base_max[i];
            data.baseline_min[i] = (base_min[i] == 0xFF) ? 0 : base_min[i];
        }
    }

    // Second pass: extract motion bytes from _motion_baseline
    // Only reliable if we have an explicit _idle_baseline (v4+), because if v < 4
    // the motion step WAS our baseline and motion bytes would just be empty.
    if (data.version >= 4 || has_idle_baseline) {
        for (const auto& r : data.results) {
            if (r.key == "_motion_baseline" && r.captured && !r.bytes.empty()) {
                for (int i = 0; i < data.report_length; ++i) {
                    const auto& bo = r.bytes[i];
                    if (bo.samples == 0) continue;
                    if ((bo.max - bo.min) <= 3) continue;
                    // Mirrors the wizard's detectCrossTalkAndWarn fix:
                    // single-bit flips during the motion-baseline step
                    // are tilt FLAGS or button bits that happened to fire
                    // while the user was tilting — not accelerometer
                    // sweeps. Falsely flagging them as motion permanently
                    // excludes them from fret detection (the CRKD in PS3
                    // mode parks its tilt flag on bit 5 of byte 0, the
                    // same byte that holds the fret bitmap, so the kit
                    // ends up with NO fret_byte).
                    const int diff = (bo.max ^ bo.min) & 0xFF;
                    const int pop = __builtin_popcount(static_cast<unsigned>(diff));
                    if (pop <= 2) continue;
                    data.motion_bytes.insert(i);
                }
            }
        }
    }
}

}  // namespace Input::HidInstrument
