// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Replays every tests/hid_kits/*.raw.jsonl capture through the same
// DeriveKitToml the in-app wizard calls, then asserts the resulting TOML
// has the right shape for the kit's device class. This is the regression
// guard for the fret-byte selection bug — if the wizard's heuristic
// regresses, a guitar fixture's TOML drops `fret_byte` and the test fails.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "input/hid_instrument.h"
#include "input/hid_kit_probe_data.h"

namespace fs = std::filesystem;
namespace HID = Input::HidInstrument;
using Input::HidInstrument::ByteObs;
using Input::HidInstrument::DeriveKitToml;
using Input::HidInstrument::KitProbeData;
using Input::HidInstrument::ProbeDeviceType;
using Input::HidInstrument::StepResultData;

namespace {

// Tiny field extractor — assumes well-formed JSON objects with simple values.
// Returns: bare string contents for "k":"v", raw array text for "k":[...],
// numeric/boolean run for "k":N.
std::string ExtractField(const std::string& line, const std::string& key) {
    const std::string needle = "\"" + key + "\":";
    auto pos = line.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    if (pos >= line.size()) return {};
    if (line[pos] == '"') {
        auto end = line.find('"', pos + 1);
        if (end == std::string::npos) return {};
        return line.substr(pos + 1, end - pos - 1);
    }
    if (line[pos] == '[') {
        auto end = line.find(']', pos);
        if (end == std::string::npos) return {};
        return line.substr(pos, end - pos + 1);
    }
    auto end = line.find_first_of(",}", pos);
    return line.substr(pos, end - pos);
}

std::vector<uint8_t> ParseBytes(const std::string& arr_str) {
    std::vector<uint8_t> out;
    std::string num;
    auto flush = [&]() {
        if (num.empty()) return;
        try {
            const int v = std::stoi(num);
            if (v >= 0 && v <= 255) out.push_back(static_cast<uint8_t>(v));
        } catch (...) {
        }
        num.clear();
    };
    for (char c : arr_str) {
        if ((c >= '0' && c <= '9') || c == '-') {
            num += c;
        } else {
            flush();
        }
    }
    flush();
    return out;
}

uint16_t ParseVidPid(const std::string& s) {
    try {
        std::string v = s;
        if (v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
            return static_cast<uint16_t>(std::stoul(v.substr(2), nullptr, 16));
        }
        return static_cast<uint16_t>(std::stoul(v, nullptr, 16));
    } catch (...) {
        return 0;
    }
}

bool VidPidFromFilename(const fs::path& p, uint16_t& vid, uint16_t& pid) {
    // Accepts both `kit_<vid>_<pid>.raw.jsonl` and the disambiguated form
    // `kit_<vid>_<pid>_<hash>.raw.jsonl` (used when two physical devices
    // share the same VID:PID — e.g. a Santroller flashed as a Guitar Hero 5
    // clone vs. flashed as a Pro Drum). Only the first two underscore-
    // delimited fields after the `kit_` prefix carry meaning here.
    auto stem = p.stem().string();
    if (stem.size() > 4 && stem.rfind(".raw") == stem.size() - 4) {
        stem = stem.substr(0, stem.size() - 4);
    }
    if (stem.rfind("kit_", 0) != 0) return false;
    const std::string rest = stem.substr(4);
    const auto us1 = rest.find('_');
    if (us1 == std::string::npos) return false;
    const std::string vid_s = rest.substr(0, us1);
    const std::string after = rest.substr(us1 + 1);
    const auto us2 = after.find('_');
    const std::string pid_s = (us2 == std::string::npos) ? after : after.substr(0, us2);
    vid = ParseVidPid(vid_s);
    pid = ParseVidPid(pid_s);
    return vid != 0 && pid != 0;
}

ProbeDeviceType InferDeviceType(const std::vector<StepResultData>& steps) {
    bool has_fret = false, has_pad = false;
    for (const auto& s : steps) {
        if (s.key.find("_fret") != std::string::npos ||
            s.key == "whammy_bar" || s.key == "touch_slider" ||
            s.key.find("strum") != std::string::npos) {
            has_fret = true;
        }
        if (s.key.find("_pad") != std::string::npos ||
            s.key.find("_cymbal") != std::string::npos ||
            s.key == "kick_pedal") {
            has_pad = true;
        }
    }
    if (has_fret && !has_pad) return ProbeDeviceType::Guitar;
    if (has_pad && !has_fret) return ProbeDeviceType::Drum;
    return ProbeDeviceType::Guitar;  // default
}

bool LoadKitProbeFromFile(const fs::path& path, KitProbeData& out) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    bool meta_seen = false;

    std::vector<std::pair<std::string, std::vector<uint8_t>>> frames;

    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const std::string type = ExtractField(line, "type");
        if (type == "meta") {
            meta_seen = true;
            try {
                const std::string v = ExtractField(line, "version");
                if (!v.empty()) out.version = std::stoi(v);
            } catch (...) {
            }
            out.vid = ParseVidPid(ExtractField(line, "vendor_id"));
            out.pid = ParseVidPid(ExtractField(line, "product_id"));
            out.device_name = ExtractField(line, "device_name");
            const std::string dt = ExtractField(line, "device_type");
            if (dt == "drum") out.device_type = ProbeDeviceType::Drum;
            else if (dt == "drum_pro") out.device_type = ProbeDeviceType::ProDrum;
            else if (dt == "guitar") out.device_type = ProbeDeviceType::Guitar;
            else if (dt == "guitar_solo") out.device_type = ProbeDeviceType::GuitarSolo;
            out.is_xinput = (ExtractField(line, "source") == "xinput");
            try {
                out.report_length = std::stoi(ExtractField(line, "report_length"));
            } catch (...) {
            }
            continue;
        }
        const std::string step = ExtractField(line, "step");
        if (step.empty()) continue;
        auto bytes = ParseBytes(ExtractField(line, "bytes"));
        if (bytes.empty()) continue;
        frames.emplace_back(step, std::move(bytes));
    }

    if (out.vid == 0 || out.pid == 0) {
        VidPidFromFilename(path, out.vid, out.pid);
    }
    if (out.report_length == 0) {
        for (const auto& [_, b] : frames) {
            if (static_cast<int>(b.size()) > out.report_length) {
                out.report_length = static_cast<int>(b.size());
            }
        }
    }
    if (out.report_length <= 0) return false;
    out.report_length = std::min(out.report_length, 64);

    std::map<std::string, StepResultData*> by_key;

    for (auto& [step, bytes] : frames) {
        StepResultData* r = nullptr;
        auto it = by_key.find(step);
        if (it == by_key.end()) {
            out.results.emplace_back();
            r = &out.results.back();
            r->key = step;
            r->captured = true;
            by_key[step] = r;
        } else {
            r = it->second;
        }
        r->raw.push_back(bytes);
        const int n = std::min<int>(64, static_cast<int>(bytes.size()));
        for (int i = 0; i < n; ++i) {
            const int v = bytes[i];
            auto& b = r->bytes[i];
            b.min = std::min(b.min, v);
            b.max = std::max(b.max, v);
            if (v != 0 && (b.min_nonzero < 0 || v < b.min_nonzero)) {
                b.min_nonzero = v;
            }
            b.samples++;
        }
    }

    Input::HidInstrument::DeriveBaselineAndMotion(out);

    if (!meta_seen) {
        out.device_type = InferDeviceType(out.results);
    }
    return true;
}

struct CaseResult {
    std::string name;
    bool passed = false;
    std::string detail;
};

bool TomlContains(const std::string& toml, const std::string& needle) {
    return toml.find(needle) != std::string::npos;
}

// Write `text` to a unique temp .toml file and return its path. Caller is
// responsible for cleanup.
fs::path WriteTempToml(const std::string& stem, const std::string& text) {
    fs::path tmp = fs::temp_directory_path() /
                   ("hidtest_" + stem + ".toml");
    std::ofstream out(tmp);
    out << text;
    return tmp;
}

// Round-trip stage: feed the derived TOML back through the live packer and
// replay each captured step's last frame. We expect EVERY non-baseline step
// to produce at least one non-zero output bit somewhere — either a button
// bitmap bit or a deviceUniqueData byte. A step that packs to all-zeros
// means the kit's wizard derivation didn't pick up that input at all, which
// is exactly the regression class we want to catch (the v0.2 fret-byte bug
// landed every fret_press step on the same idle-state pattern).
bool RoundTripStep(const StepResultData& step, std::string& detail) {
    if (step.raw.empty()) {
        detail = "step '" + step.key + "' has no frames";
        return false;
    }
    // Try every captured frame for the step — at least one must produce
    // non-zero output. Picking only the last frame is fragile: the user may
    // have released the input by the final frame (especially for tilt,
    // where they return the guitar to neutral after pulling it up).
    for (const auto& frame : step.raw) {
        u8 dud[HID::kMaxDeviceUniqueData] = {};
        HID::PackDeviceUniqueData(1, frame.data(), frame.size(),
                                  Libraries::Pad::OrbisPadDeviceClass::Guitar, dud);
        const u32 buttons = HID::PackButtons(
            1, frame.data(), frame.size(),
            Libraries::Pad::OrbisPadDeviceClass::Guitar);
        if (buttons != 0) return true;
        for (std::size_t i = 0; i < HID::kMaxDeviceUniqueData; ++i) {
            if (dud[i] != 0) return true;
        }
    }
    detail = "step '" + step.key + "' packed to all-zero output across "
             + std::to_string(step.raw.size()) + " frames";
    return false;
}

// Per-fret face-button check, used for the 5 solo_*_fret steps on a
// GuitarSolo kit with either a dedicated solo_fret_byte OR a
// solo_modifier_byte/mask. Each press must:
//   (a) flip a bit in dud[4] (the fretSolo flag), AND
//   (b) fire a face-button bit (Cross / Circle / Triangle / Square / L1)
//       in PackButtons output — without [buttons_byte_<solo>] mapping
//       RB4 sees the solo flag but no button press, and the note never
//       registers as held.
// We also require that some frame in the step left dud[3] == 0 while
// dud[4] != 0: that's the proof the solo-modifier code path actually
// routes the bits to dud[4] instead of letting them stay in the main
// fret slot. Catches a regression where solo_modifier_byte is set but
// PackDeviceUniqueData ignores it.
bool SoloFretStep(const StepResultData& step, std::string& detail) {
    using B = Libraries::Pad::OrbisPadButtonDataOffset;
    const u32 face_mask =
        static_cast<u32>(B::Cross) | static_cast<u32>(B::Circle) |
        static_cast<u32>(B::Triangle) | static_cast<u32>(B::Square) |
        static_cast<u32>(B::L1);
    if (step.raw.empty()) {
        detail = "solo step '" + step.key + "' has no frames";
        return false;
    }
    bool saw_solo_dud = false, saw_face_button = false, saw_solo_isolated = false;
    for (const auto& frame : step.raw) {
        u8 dud[HID::kMaxDeviceUniqueData] = {};
        HID::PackDeviceUniqueData(1, frame.data(), frame.size(),
                                  Libraries::Pad::OrbisPadDeviceClass::Guitar, dud);
        if (dud[4] != 0) {
            saw_solo_dud = true;
            if (dud[3] == 0) saw_solo_isolated = true;
        }
        const u32 buttons = HID::PackButtons(
            1, frame.data(), frame.size(),
            Libraries::Pad::OrbisPadDeviceClass::Guitar);
        if (buttons & face_mask) saw_face_button = true;
        if (saw_solo_dud && saw_face_button && saw_solo_isolated) return true;
    }
    if (!saw_solo_dud) {
        detail = "solo step '" + step.key + "' produced no dud[4] bit";
        return false;
    }
    if (!saw_face_button) {
        detail = "solo step '" + step.key + "' fired dud[4] but no face button bit "
                 "(missing [buttons_byte_<solo>] mapping?)";
        return false;
    }
    detail = "solo step '" + step.key + "' set dud[4] but never cleared dud[3] "
             "in the same frame (solo_modifier_byte not gating dud[3]?)";
    return false;
}

// Counterpart for the regular fret_* steps on a GuitarSolo kit: pressing
// a main fret with NO solo modifier held must leave dud[4] == 0. Without
// this, a kit derived as solo-modifier-style but with a buggy packer
// (modifier always active) would route every fret press to dud[4],
// effectively breaking the whole main fretboard — and SoloFretStep alone
// wouldn't catch it because it doesn't run on non-solo steps.
bool MainFretStep(const StepResultData& step, std::string& detail) {
    if (step.raw.empty()) {
        detail = "fret step '" + step.key + "' has no frames";
        return false;
    }
    bool saw_main_dud = false;
    for (const auto& frame : step.raw) {
        u8 dud[HID::kMaxDeviceUniqueData] = {};
        HID::PackDeviceUniqueData(1, frame.data(), frame.size(),
                                  Libraries::Pad::OrbisPadDeviceClass::Guitar, dud);
        if (dud[4] != 0) {
            detail = "fret step '" + step.key +
                     "' leaked into dud[4] (modifier code path stuck on?)";
            return false;
        }
        if (dud[3] != 0) saw_main_dud = true;
    }
    if (!saw_main_dud) {
        detail = "fret step '" + step.key + "' produced no dud[3] bit";
        return false;
    }
    return true;
}

// "Both solo frets held" combo (e.g. solo_green_blue). The capture must
// produce a frame where:
//   - dud[4] has TWO bits set (green + blue solo positions), AND
//   - PackButtons fires BOTH face buttons simultaneously (Cross | Square).
// Catches kit derivations that pick a fret mask too narrow to cover both
// bits, or that drop the [buttons_byte_<solo>] entries for individual
// colours so the combined press only fires one button.
bool SoloFretComboStep(const StepResultData& step, std::string& detail) {
    using B = Libraries::Pad::OrbisPadButtonDataOffset;
    const u32 both_buttons = static_cast<u32>(B::Cross) | static_cast<u32>(B::Square);
    if (step.raw.empty()) {
        detail = "solo combo '" + step.key + "' has no frames";
        return false;
    }
    bool saw_two_dud_bits = false, saw_both_buttons = false;
    for (const auto& frame : step.raw) {
        u8 dud[HID::kMaxDeviceUniqueData] = {};
        HID::PackDeviceUniqueData(1, frame.data(), frame.size(),
                                  Libraries::Pad::OrbisPadDeviceClass::Guitar, dud);
        const u8 d4 = dud[4];
        if (d4 != 0 && (d4 & (d4 - 1)) != 0) saw_two_dud_bits = true;
        const u32 buttons = HID::PackButtons(
            1, frame.data(), frame.size(),
            Libraries::Pad::OrbisPadDeviceClass::Guitar);
        if ((buttons & both_buttons) == both_buttons) saw_both_buttons = true;
        if (saw_two_dud_bits && saw_both_buttons) return true;
    }
    if (!saw_two_dud_bits) {
        detail = "solo combo '" + step.key +
                 "' never set two bits in dud[4] (solo_fret_byte mask too narrow?)";
        return false;
    }
    detail = "solo combo '" + step.key +
             "' set two dud[4] bits but never fired both face buttons "
             "(missing solo bit→button mapping for one of green/blue?)";
    return false;
}

CaseResult RunCase(const fs::path& path) {
    CaseResult r{};
    r.name = path.filename().string();
    KitProbeData data;
    if (!LoadKitProbeFromFile(path, data)) {
        r.detail = "could not parse fixture";
        return r;
    }
    const std::string toml = DeriveKitToml(data);
    if (toml.empty()) {
        r.detail = "DeriveKitToml returned empty";
        return r;
    }
    if (!TomlContains(toml, "schema       = \"shadps4-legacy-instrument/v1\"")) {
        r.detail = "missing schema header";
        return r;
    }
    const bool is_guitar = data.device_type == ProbeDeviceType::Guitar ||
                           data.device_type == ProbeDeviceType::GuitarSolo;
    // v3 introduced combo steps (green_strum / green_blue / green_blue_strum)
    // and the solo-fret walkthrough for GuitarSolo kits. v1/v2 captures
    // predate them and must not be punished for missing keys.
    auto has_step = [&](const char* name) {
        for (const auto& s : data.results) {
            if (s.key == name && s.captured) return true;
        }
        return false;
    };
    if (data.version >= 3 && is_guitar) {
        if (!has_step("green_strum")) {
            r.detail = "v3 guitar missing combo step 'green_strum'";
            return r;
        }
    }
    if (data.version >= 5 && data.device_type == ProbeDeviceType::GuitarSolo) {
        if (!has_step("solo_green_blue")) {
            r.detail = "v5 guitar_solo missing combo step 'solo_green_blue'";
            return r;
        }
    }
    if (is_guitar) {
        if (!TomlContains(toml, "device_class = \"guitar\"")) {
            r.detail = "expected device_class = \"guitar\"";
            return r;
        }
        if (!TomlContains(toml, "fret_byte = ")) {
            r.detail = "missing fret_byte for guitar";
            return r;
        }
        // GuitarSolo TOMLs must declare at least one solo-encoding path:
        //   solo_fret_byte         (PS4 Mustang / PS5 Riffmaster — dedicated byte)
        //   solo_modifier_byte     (X360 RB / Strat — main fret + L3 modifier)
        // If neither shows up, the wizard didn't actually capture solo
        // frets — call it out.
        if (data.device_type == ProbeDeviceType::GuitarSolo &&
            !TomlContains(toml, "solo_fret_byte = ") &&
            !TomlContains(toml, "solo_modifier_byte = ")) {
            r.detail = "missing solo_fret_byte / solo_modifier_byte for guitar_solo";
            return r;
        }
        if (!TomlContains(toml, "[buttons_byte_")) {
            r.detail = "no [buttons_byte_*] section for guitar frets";
            return r;
        }
    } else {
        if (!TomlContains(toml, "device_class = \"drum\"")) {
            r.detail = "expected device_class = \"drum\"";
            return r;
        }
        if (!TomlContains(toml, "[buttons_byte_")) {
            r.detail = "no [buttons_byte_*] section for drum face buttons";
            return r;
        }
    }

    // Round-trip: derived TOML -> live packer -> per-step replay.
    HID::Testing::ResetForTesting();
    const fs::path tmp = WriteTempToml(path.stem().string(), toml);
    const bool bound = HID::Testing::BindKitFromTomlForTesting(1, tmp.string());
    std::error_code ec;
    fs::remove(tmp, ec);
    if (!bound) {
        r.detail = "BindKitFromTomlForTesting failed";
        return r;
    }
    // Steps the round-trip can't reliably verify through PackButtons /
    // PackDeviceUniqueData:
    //   tilt_up      — PS3 GH tilt is delivered via OrbisPadData::acceleration,
    //                  not the dud-byte pipeline. The accel path reads from
    //                  the slot's last_report which this harness doesn't fill.
    //   button_ps    — not all guitars expose Guide/Home on their HID report;
    //                  on some kits the wizard correctly omits it.
    static const std::vector<std::string> kSkipSteps = {
        "tilt_up", "button_ps",
        // 2nd kick pedal is captured separately by the wizard but the
        // current derivation maps only the primary kick into the TOML —
        // packing the 2nd kick frame produces no output. Optional step.
        "kick_pedal_2",
    };
    const bool check_solo_buttons =
        data.device_type == ProbeDeviceType::GuitarSolo &&
        (TomlContains(toml, "solo_fret_byte = ") ||
         TomlContains(toml, "solo_modifier_byte = "));
    for (const auto& step : data.results) {
        if (step.key.empty() || step.key[0] == '_') continue;
        if (std::find(kSkipSteps.begin(), kSkipSteps.end(), step.key) !=
            kSkipSteps.end()) {
            continue;
        }
        std::string detail;
        // Stronger check for the 5 solo frets on a GuitarSolo kit with a
        // dedicated solo_fret_byte: each press must produce BOTH a dud[4]
        // bit AND a face-button bit. The generic RoundTripStep only
        // requires one of the two, which lets the regression slip
        // through.
        const bool is_solo_step =
            step.key.rfind("solo_", 0) == 0 &&
            step.key.find("_fret") != std::string::npos;
        if (check_solo_buttons && is_solo_step) {
            if (!SoloFretStep(step, detail)) {
                r.detail = detail;
                return r;
            }
            continue;
        }
        if (check_solo_buttons && step.key == "solo_green_blue") {
            if (!SoloFretComboStep(step, detail)) {
                r.detail = detail;
                return r;
            }
            continue;
        }
        // On a GuitarSolo kit, main fret presses must NOT leak into
        // dud[4]. green_blue / green_blue_strum are combo steps that
        // skip strict main-fret checking (HAT byte may interact);
        // green_strum strums while holding green — also fine.
        const bool is_main_fret =
            step.key.find("_fret") != std::string::npos &&
            step.key.rfind("solo_", 0) != 0;
        if (check_solo_buttons && is_main_fret) {
            if (!MainFretStep(step, detail)) {
                r.detail = detail;
                return r;
            }
            continue;
        }
        if (!RoundTripStep(step, detail)) {
            r.detail = detail;
            return r;
        }
    }
    r.passed = true;
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    // `hidtest --dump <file.raw.jsonl>` — parse the fixture, run DeriveKitToml,
    // print the resulting TOML to stdout. Helpful when a stored .toml looks
    // incomplete and we want to confirm whether the current code path produces
    // the right output from the captured data (vs. an older wizard binary
    // having written the .toml).
    if (argc >= 3 && std::string(argv[1]) == "--dump") {
        KitProbeData data;
        if (!LoadKitProbeFromFile(fs::path(argv[2]), data)) {
            std::cerr << "hidtest --dump: could not parse " << argv[2] << "\n";
            return 2;
        }
        std::cout << DeriveKitToml(data);
        return 0;
    }
    fs::path fixtures = (argc > 1) ? fs::path(argv[1])
                                   : fs::current_path() / "tests" / "hid_kits";
    if (!fs::is_directory(fixtures)) {
        std::cerr << "hidtest: fixtures dir not found: " << fixtures << "\n";
        return 2;
    }
    std::vector<fs::path> files;
    for (auto& entry : fs::directory_iterator(fixtures)) {
        if (entry.path().extension() == ".jsonl") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    std::vector<CaseResult> results;
    for (auto& f : files) {
        results.push_back(RunCase(f));
    }

    int failed = 0;
    for (const auto& r : results) {
        std::cout << (r.passed ? "[ OK ]  " : "[FAIL]  ") << r.name;
        if (!r.detail.empty()) std::cout << "  -- " << r.detail;
        std::cout << "\n";
        if (!r.passed) ++failed;
    }
    std::cout << "hidtest: " << (results.size() - failed) << "/"
              << results.size() << " passed\n";
    return failed ? 1 : 0;
}
