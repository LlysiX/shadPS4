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
#include <map>
#include <sstream>
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

std::string ExtractField(const std::string &line, const std::string &key) {
  const std::string needle = "\"" + key + "\":";
  auto pos = line.find(needle);
  if (pos == std::string::npos)
    return {};
  pos += needle.size();
  if (pos >= line.size())
    return {};
  if (line[pos] == '"') {
    auto end = line.find('"', pos + 1);
    if (end == std::string::npos)
      return {};
    return line.substr(pos + 1, end - pos - 1);
  }
  if (line[pos] == '[') {
    // Return everything from '[' through its matching ']'.
    int depth = 0;
    std::size_t end = pos;
    for (; end < line.size(); ++end) {
      if (line[end] == '[')
        ++depth;
      else if (line[end] == ']' && --depth == 0) {
        ++end;
        break;
      }
    }
    return line.substr(pos, end - pos);
  }
  if (line[pos] == '{') {
    // Return everything from '{' through its matching '}'. Brace-depth
    // tracked so a nested object inside the value doesn't terminate
    // the slice early.
    int depth = 0;
    std::size_t end = pos;
    for (; end < line.size(); ++end) {
      if (line[end] == '{')
        ++depth;
      else if (line[end] == '}' && --depth == 0) {
        ++end;
        break;
      }
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
std::vector<MidiEvent> ParseEvents(const std::string &arr) {
  std::vector<MidiEvent> out;
  std::size_t i = 0;
  while ((i = arr.find('{', i)) != std::string::npos) {
    std::size_t end = arr.find('}', i);
    if (end == std::string::npos)
      break;
    const std::string obj = arr.substr(i, end - i + 1);
    MidiEvent ev;
    const std::string on_s = ExtractField(obj, "on");
    ev.on = (on_s == "true" || on_s == "1");
    try {
      ev.note = static_cast<std::uint8_t>(std::stoi(ExtractField(obj, "note")));
    } catch (...) {
    }
    try {
      ev.velocity =
          static_cast<std::uint8_t>(std::stoi(ExtractField(obj, "vel")));
    } catch (...) {
    }
    try {
      ev.t_ms =
          static_cast<std::uint32_t>(std::stoul(ExtractField(obj, "t_ms")));
    } catch (...) {
    }
    out.push_back(ev);
    i = end + 1;
  }
  return out;
}

// Parse an inline JSON object like {"36":"red","38":"yellow",...} into
// a (note → pad-name) vector. Tolerant of whitespace; keys may be
// quoted strings holding decimal integers (the wizard writes them that
// way since JSON object keys must be strings).
std::vector<std::pair<int, std::string>>
ParseExpectedPad(const std::string &obj) {
  std::vector<std::pair<int, std::string>> out;
  std::size_t i = 0;
  while ((i = obj.find('"', i)) != std::string::npos) {
    auto key_end = obj.find('"', i + 1);
    if (key_end == std::string::npos)
      break;
    const std::string key = obj.substr(i + 1, key_end - i - 1);
    auto colon = obj.find(':', key_end);
    if (colon == std::string::npos)
      break;
    auto val_start = obj.find('"', colon);
    if (val_start == std::string::npos)
      break;
    auto val_end = obj.find('"', val_start + 1);
    if (val_end == std::string::npos)
      break;
    const std::string val = obj.substr(val_start + 1, val_end - val_start - 1);
    try {
      out.emplace_back(std::stoi(key), val);
    } catch (...) {
    }
    i = val_end + 1;
  }
  return out;
}

// Like ParseExpectedPad but the JSON values are integers: {"red":12,...}.
// Returns (pad-name, expected gate value) pairs.
std::vector<std::pair<std::string, int>>
ParseExpectedGate(const std::string &obj) {
  std::vector<std::pair<std::string, int>> out;
  std::size_t i = 0;
  while ((i = obj.find('"', i)) != std::string::npos) {
    auto key_end = obj.find('"', i + 1);
    if (key_end == std::string::npos)
      break;
    const std::string key = obj.substr(i + 1, key_end - i - 1);
    auto colon = obj.find(':', key_end);
    if (colon == std::string::npos)
      break;
    auto end = obj.find_first_of(",}", colon);
    if (end == std::string::npos)
      end = obj.size();
    try {
      const int v = std::stoi(obj.substr(colon + 1, end - colon - 1));
      out.emplace_back(key, v);
    } catch (...) {
    }
    i = end + 1;
  }
  return out;
}

bool LoadMidiCapture(const fs::path &path, MidiKitProbeData &out,
                     std::vector<std::pair<int, std::string>> &expected_pad,
                     std::vector<std::pair<std::string, int>> &expected_gate) {
  std::ifstream in(path);
  if (!in)
    return false;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty())
      continue;
    if (ExtractField(line, "type") == "meta") {
      out.device_name = ExtractField(line, "device_name");
      out.port_id = ExtractField(line, "port_id");
      out.timestamp = ExtractField(line, "timestamp");
      out.capture_uuid = ExtractField(line, "capture_uuid");
      out.host_hash = ExtractField(line, "host_hash");
      const std::string dt = ExtractField(line, "device_type");
      out.device_type =
          (dt == "pro_drum") ? MidiDeviceType::ProDrum : MidiDeviceType::Drum;
      try {
        out.version = std::stoi(ExtractField(line, "version"));
      } catch (...) {
      }
      const std::string ep = ExtractField(line, "expected_pad");
      if (!ep.empty())
        expected_pad = ParseExpectedPad(ep);
      const std::string eg = ExtractField(line, "expected_gate");
      if (!eg.empty())
        expected_gate = ParseExpectedGate(eg);
      continue;
    }
    const std::string step = ExtractField(line, "step");
    if (step.empty())
      continue;
    MidiStepResult r;
    r.key = step;
    r.captured = true;
    const std::string ev_arr = ExtractField(line, "events");
    if (!ev_arr.empty())
      r.events = ParseEvents(ev_arr);
    out.results.push_back(std::move(r));
  }
  return !out.results.empty();
}

bool Contains(const std::string &s, const std::string &needle) {
  return s.find(needle) != std::string::npos;
}

struct CaseResult {
  std::string name;
  bool passed = false;
  std::string detail;
};

CaseResult RunCase(const fs::path &path) {
  CaseResult r{};
  r.name = path.filename().string();
  MidiKitProbeData data;
  std::vector<std::pair<int, std::string>> expected_pad;
  std::vector<std::pair<std::string, int>> expected_gate;
  if (!LoadMidiCapture(path, data, expected_pad, expected_gate)) {
    r.detail = "could not parse fixture";
    return r;
  }
  const std::string toml = DeriveMidiKitToml(data);
  if (toml.empty()) {
    r.detail = "DeriveMidiKitToml returned empty";
    return r;
  }
  if (!Contains(toml, "schema       = \"shadps4-midi-instrument/v")) {
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
  static const std::pair<const char *, const char *> kStepToTomlKey[] = {
      {"red_pad", "red ="},
      {"blue_pad", "blue ="},
      {"yellow_pad", "yellow ="},
      {"green_pad", "green ="},
      {"kick_pedal", "kick ="},
      {"yellow_cymbal", "yellow_cymbal ="},
      {"blue_cymbal", "blue_cymbal ="},
      {"green_cymbal", "green_cymbal ="},
  };
  // Note: We no longer assert that a step with events MUST appear in the TOML.
  // Cross-talk dedup or all-zero velocity captures legitimately exclude steps.
  // A MIDI note must not appear under more than one pad. The original
  // last-writer-wins parse silently swapped pads when the wizard
  // captured cross-talk between adjacent steps; we now dedup at derive
  // time so the same note never lands in two lists.
  std::map<int, std::string> note_owner;
  {
    std::istringstream iss(toml);
    std::string line;
    bool in_map = false;
    while (std::getline(iss, line)) {
      while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
        line.erase(line.begin());
      while (!line.empty() &&
             (line.back() == ' ' || line.back() == '\n' || line.back() == '\t'))
        line.pop_back();
      if (line.empty())
        continue;
      if (line == "[midi_pad_map]") {
        in_map = true;
        continue;
      }
      if (line.front() == '[') {
        in_map = false;
        continue;
      }
      if (!in_map)
        continue;
      const auto eq = line.find('=');
      const auto lbr = line.find('[', eq);
      const auto rbr = line.find(']', lbr);
      if (eq == std::string::npos || lbr == std::string::npos ||
          rbr == std::string::npos)
        continue;
      std::string pad = line.substr(0, eq);
      while (!pad.empty() && pad.back() == ' ')
        pad.pop_back();
      std::string toks = line.substr(lbr + 1, rbr - lbr - 1);
      std::istringstream ns(toks);
      std::string tok;
      while (std::getline(ns, tok, ',')) {
        while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t'))
          tok.erase(tok.begin());
        while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t'))
          tok.pop_back();
        if (tok.empty())
          continue;
        try {
          int n = std::stoi(tok);
          auto it = note_owner.find(n);
          if (it != note_owner.end() && it->second != pad) {
            r.detail = "note " + std::to_string(n) + " appears under both '" +
                       it->second + "' and '" + pad + "' (dedup broken)";
            return r;
          }
          note_owner[n] = pad;
        } catch (...) {
        }
      }
    }
  }
  // Captures with explicit "expected_pad" hints in their meta header
  // pin the assignment: each note must land under exactly the named
  // pad in the derived TOML's [midi_pad_map]. Community captures carry
  // hints derived from the user-confirmed TOML, and synthesised
  // fixtures carry hints picked by the script that wrote them.
  for (const auto &[note, pad] : expected_pad) {
    auto it = note_owner.find(note);
    if (it == note_owner.end()) {
      r.detail = std::string{"expected note "} + std::to_string(note) +
                 " under '" + pad + "' but it wasn't emitted";
      return r;
    }
    if (it->second != pad) {
      r.detail = std::string{"note "} + std::to_string(note) +
                 " landed under '" + it->second + "', expected '" + pad + "'";
      return r;
    }
  }

  // /v2 [gate] assertion: parse the derived TOML's gate block and
  // compare each pad's value against the meta's expected_gate hint.
  // Captures without expected_gate (most existing fixtures) skip this
  // check entirely.
  if (!expected_gate.empty()) {
    std::map<std::string, int> emitted_gate;
    std::istringstream iss(toml);
    std::string line;
    bool in_gate = false;
    while (std::getline(iss, line)) {
      while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
        line.erase(line.begin());
      while (!line.empty() &&
             (line.back() == ' ' || line.back() == '\n' || line.back() == '\t'))
        line.pop_back();
      if (line.empty())
        continue;
      if (line == "[gate]") {
        in_gate = true;
        continue;
      }
      if (line.front() == '[') {
        in_gate = false;
        continue;
      }
      if (!in_gate)
        continue;
      // Lines look like:  "red" = 12
      const auto eq = line.find('=');
      if (eq == std::string::npos)
        continue;
      std::string key = line.substr(0, eq);
      while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
        key.pop_back();
      if (key.size() >= 2 && key.front() == '"' && key.back() == '"')
        key = key.substr(1, key.size() - 2);
      try {
        const int v = std::stoi(line.substr(eq + 1));
        emitted_gate[key] = v;
      } catch (...) {
      }
    }
    for (const auto &[pad, expected] : expected_gate) {
      auto it = emitted_gate.find(pad);
      if (it == emitted_gate.end()) {
        r.detail = "expected [gate] entry for '" + pad +
                   "' = " + std::to_string(expected) + " but none emitted";
        return r;
      }
      if (it->second != expected) {
        r.detail = "[gate] '" + pad + "' emitted as " +
                   std::to_string(it->second) + ", expected " +
                   std::to_string(expected);
        return r;
      }
    }
  }
  r.passed = true;
  return r;
}

} // namespace

int main(int argc, char **argv) {
  // Default fixtures root is tests/midi — the walker is recursive so it
  // picks up both synthesised/ (Python-generated edge cases) and
  // community/ (real-hardware captures with expected_pad hints).
  fs::path fixtures =
      (argc > 1) ? fs::path(argv[1]) : fs::current_path() / "tests" / "midi";
  if (!fs::is_directory(fixtures)) {
    std::cerr << "miditest: fixtures dir not found: " << fixtures << "\n";
    return 2;
  }
  std::vector<fs::path> files;
  for (auto &entry : fs::recursive_directory_iterator(fixtures)) {
    if (entry.path().extension() == ".jsonl")
      files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());

  std::vector<CaseResult> results;
  for (auto &f : files)
    results.push_back(RunCase(f));

  int failed = 0;
  for (const auto &r : results) {
    std::cout << (r.passed ? "[ OK ]  " : "[FAIL]  ") << r.name;
    if (!r.detail.empty())
      std::cout << "  -- " << r.detail;
    std::cout << "\n";
    if (!r.passed)
      ++failed;
  }
  std::cout << "miditest: " << (results.size() - failed) << "/"
            << results.size() << " passed\n";
  return failed ? 1 : 0;
}
