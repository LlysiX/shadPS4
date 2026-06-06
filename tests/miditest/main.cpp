// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Replays every tests/midi/synthesised/*.midi.jsonl (and any community-
// captured fixtures dropped alongside them) through DeriveMidiKitToml
// and asserts the resulting TOML has the right shape for the kit's
// declared device class. Companion to tests/hidtest/main.cpp.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "input/midi_kit_probe_data.h"

namespace fs = std::filesystem;
using Input::MidiInstrument::DeriveMidiKitToml;
using Input::MidiInstrument::MidiDeviceType;
using Input::MidiInstrument::MidiEvent;
using Input::MidiInstrument::MidiKitProbeData;
using Input::MidiInstrument::MidiStepResult;

namespace {

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
        // Return everything from '[' through its matching ']'.
        int depth = 0;
        std::size_t end = pos;
        for (; end < line.size(); ++end) {
            if (line[end] == '[') ++depth;
            else if (line[end] == ']' && --depth == 0) { ++end; break; }
        }
        return line.substr(pos, end - pos);
    }
    auto end = line.find_first_of(",}", pos);
    return line.substr(pos, end - pos);
}

// Parse a JSON events array of the form
// [{"on":true,"note":38,"vel":92,"t_ms":18}, ...]
// into a vector<MidiEvent>. Hand-rolled because the project doesn't
// pull a JSON parser and the format is well-bounded.
std::vector<MidiEvent> ParseEvents(const std::string& arr) {
    std::vector<MidiEvent> out;
    std::size_t i = 0;
    while ((i = arr.find('{', i)) != std::string::npos) {
        std::size_t end = arr.find('}', i);
        if (end == std::string::npos) break;
        const std::string obj = arr.substr(i, end - i + 1);
        MidiEvent ev;
        const std::string on_s = ExtractField(obj, "on");
        ev.on = (on_s == "true" || on_s == "1");
        try { ev.note = static_cast<std::uint8_t>(std::stoi(ExtractField(obj, "note"))); }
        catch (...) {}
        try { ev.velocity = static_cast<std::uint8_t>(std::stoi(ExtractField(obj, "vel"))); }
        catch (...) {}
        try { ev.t_ms = static_cast<std::uint32_t>(std::stoul(ExtractField(obj, "t_ms"))); }
        catch (...) {}
        out.push_back(ev);
        i = end + 1;
    }
    return out;
}

bool LoadMidiCapture(const fs::path& path, MidiKitProbeData& out) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (ExtractField(line, "type") == "meta") {
            out.device_name = ExtractField(line, "device_name");
            out.port_id = ExtractField(line, "port_id");
            out.timestamp = ExtractField(line, "timestamp");
            out.capture_uuid = ExtractField(line, "capture_uuid");
            out.host_hash = ExtractField(line, "host_hash");
            const std::string dt = ExtractField(line, "device_type");
            out.device_type = (dt == "pro_drum") ? MidiDeviceType::ProDrum
                                                 : MidiDeviceType::Drum;
            try { out.version = std::stoi(ExtractField(line, "version")); }
            catch (...) {}
            continue;
        }
        const std::string step = ExtractField(line, "step");
        if (step.empty()) continue;
        MidiStepResult r;
        r.key = step;
        r.captured = true;
        const std::string ev_arr = ExtractField(line, "events");
        if (!ev_arr.empty()) r.events = ParseEvents(ev_arr);
        out.results.push_back(std::move(r));
    }
    return !out.results.empty();
}

bool Contains(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

struct CaseResult {
    std::string name;
    bool passed = false;
    std::string detail;
};

CaseResult RunCase(const fs::path& path) {
    CaseResult r{};
    r.name = path.filename().string();
    MidiKitProbeData data;
    if (!LoadMidiCapture(path, data)) {
        r.detail = "could not parse fixture";
        return r;
    }
    const std::string toml = DeriveMidiKitToml(data);
    if (toml.empty()) {
        r.detail = "DeriveMidiKitToml returned empty";
        return r;
    }
    if (!Contains(toml, "schema       = \"shadps4-midi-instrument/v1\"")) {
        r.detail = "missing midi schema header";
        return r;
    }
    if (!Contains(toml, "[midi_pad_map]")) {
        r.detail = "missing [midi_pad_map] section";
        return r;
    }
    // Every step the capture has a non-empty events list for MUST appear
    // in the pad map. Catches the failure mode where the wizard captured
    // a pad but DeriveMidiKitToml's note-ranking returns empty for it.
    static const std::pair<const char*, const char*> kStepToTomlKey[] = {
        {"red_pad",        "red ="},
        {"blue_pad",       "blue ="},
        {"yellow_pad",     "yellow ="},
        {"green_pad",      "green ="},
        {"kick_pedal",     "kick ="},
        {"yellow_cymbal",  "yellow_cymbal ="},
        {"blue_cymbal",    "blue_cymbal ="},
        {"green_cymbal",   "green_cymbal ="},
    };
    for (const auto& s : data.results) {
        if (s.events.empty()) continue;
        for (const auto& [step_key, toml_key] : kStepToTomlKey) {
            if (s.key != step_key) continue;
            if (!Contains(toml, toml_key)) {
                r.detail = std::string{"step '"} + step_key +
                           "' had events but didn't appear in pad map";
                return r;
            }
            break;
        }
    }
    r.passed = true;
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    fs::path fixtures = (argc > 1) ? fs::path(argv[1])
                                   : fs::current_path() / "tests" / "midi" /
                                         "synthesised";
    if (!fs::is_directory(fixtures)) {
        std::cerr << "miditest: fixtures dir not found: " << fixtures << "\n";
        return 2;
    }
    std::vector<fs::path> files;
    for (auto& entry : fs::recursive_directory_iterator(fixtures)) {
        if (entry.path().extension() == ".jsonl") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());

    std::vector<CaseResult> results;
    for (auto& f : files) results.push_back(RunCase(f));

    int failed = 0;
    for (const auto& r : results) {
        std::cout << (r.passed ? "[ OK ]  " : "[FAIL]  ") << r.name;
        if (!r.detail.empty()) std::cout << "  -- " << r.detail;
        std::cout << "\n";
        if (!r.passed) ++failed;
    }
    std::cout << "miditest: " << (results.size() - failed) << "/"
              << results.size() << " passed\n";
    return failed ? 1 : 0;
}
