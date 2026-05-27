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

#include "input/hid_kit_probe_data.h"

namespace fs = std::filesystem;
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
    auto stem = p.stem().string();  // kit_VVVV_PPPP.raw -> stem strips .jsonl only
    if (stem.size() > 4 && stem.rfind(".raw") == stem.size() - 4) {
        stem = stem.substr(0, stem.size() - 4);
    }
    if (stem.rfind("kit_", 0) != 0) return false;
    const std::string rest = stem.substr(4);
    const auto us = rest.find('_');
    if (us == std::string::npos) return false;
    vid = ParseVidPid(rest.substr(0, us));
    pid = ParseVidPid(rest.substr(us + 1));
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
            out.vid = ParseVidPid(ExtractField(line, "vendor_id"));
            out.pid = ParseVidPid(ExtractField(line, "product_id"));
            out.device_name = ExtractField(line, "device_name");
            const std::string dt = ExtractField(line, "device_type");
            if (dt == "drum") out.device_type = ProbeDeviceType::Drum;
            else if (dt == "drum_pro") out.device_type = ProbeDeviceType::ProDrum;
            else if (dt == "guitar") out.device_type = ProbeDeviceType::Guitar;
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

    std::array<int, 64> base_max{};
    std::array<int, 64> base_min{};
    base_min.fill(0xFF);
    bool have_baseline = false;
    std::map<std::string, StepResultData*> by_key;

    for (auto& [step, bytes] : frames) {
        if (step == "_motion_baseline") {
            have_baseline = true;
            const int n = std::min<int>(64, static_cast<int>(bytes.size()));
            for (int i = 0; i < n; ++i) {
                base_max[i] = std::max(base_max[i], static_cast<int>(bytes[i]));
                base_min[i] = std::min(base_min[i], static_cast<int>(bytes[i]));
            }
            continue;
        }
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

    if (have_baseline) {
        for (int i = 0; i < 64; ++i) {
            out.baseline_max[i] = base_max[i];
            out.baseline_min[i] = (base_min[i] == 0xFF) ? 0 : base_min[i];
        }
    }

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
    if (data.device_type == ProbeDeviceType::Guitar) {
        if (!TomlContains(toml, "device_class = \"guitar\"")) {
            r.detail = "expected device_class = \"guitar\"";
            return r;
        }
        if (!TomlContains(toml, "fret_byte = ")) {
            r.detail = "missing fret_byte for guitar";
            return r;
        }
        // Frets MUST land in a [buttons_byte_*] map.
        if (!TomlContains(toml, "[buttons_byte_")) {
            r.detail = "no [buttons_byte_*] section for guitar frets";
            return r;
        }
    } else {
        if (!TomlContains(toml, "device_class = \"drum\"")) {
            r.detail = "expected device_class = \"drum\"";
            return r;
        }
        if (!TomlContains(toml, "[buttons_byte_0]")) {
            r.detail = "missing [buttons_byte_0] for drum face buttons";
            return r;
        }
    }
    r.passed = true;
    return r;
}

}  // namespace

int main(int argc, char** argv) {
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
