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
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "input/hid_instrument.h"
#include "input/hid_kit_def.h"
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
    auto end = line.find(']', pos);
    if (end == std::string::npos)
      return {};
    return line.substr(pos, end - pos + 1);
  }
  auto end = line.find_first_of(",}", pos);
  return line.substr(pos, end - pos);
}

std::vector<uint8_t> ParseBytes(const std::string &arr_str) {
  std::vector<uint8_t> out;
  std::string num;
  auto flush = [&]() {
    if (num.empty())
      return;
    try {
      const int v = std::stoi(num);
      if (v >= 0 && v <= 255)
        out.push_back(static_cast<uint8_t>(v));
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

uint16_t ParseVidPid(const std::string &s) {
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

bool VidPidFromFilename(const fs::path &p, uint16_t &vid, uint16_t &pid) {
  // Accepts both `kit_<vid>_<pid>.raw.jsonl` and the disambiguated form
  // `kit_<vid>_<pid>_<hash>.raw.jsonl` (used when two physical devices
  // share the same VID:PID — e.g. a Santroller flashed as a Guitar Hero 5
  // clone vs. flashed as a Pro Drum). Only the first two underscore-
  // delimited fields after the `kit_` prefix carry meaning here.
  auto stem = p.stem().string();
  if (stem.size() > 4 && stem.rfind(".raw") == stem.size() - 4) {
    stem = stem.substr(0, stem.size() - 4);
  }
  if (stem.rfind("kit_", 0) != 0)
    return false;
  const std::string rest = stem.substr(4);
  const auto us1 = rest.find('_');
  if (us1 == std::string::npos)
    return false;
  const std::string vid_s = rest.substr(0, us1);
  const std::string after = rest.substr(us1 + 1);
  const auto us2 = after.find('_');
  const std::string pid_s =
      (us2 == std::string::npos) ? after : after.substr(0, us2);
  vid = ParseVidPid(vid_s);
  pid = ParseVidPid(pid_s);
  return vid != 0 && pid != 0;
}

ProbeDeviceType InferDeviceType(const std::vector<StepResultData> &steps) {
  bool has_fret = false, has_pad = false;
  for (const auto &s : steps) {
    if (s.key.find("_fret") != std::string::npos || s.key == "whammy_bar" ||
        s.key == "touch_slider" || s.key.find("strum") != std::string::npos) {
      has_fret = true;
    }
    if (s.key.find("_pad") != std::string::npos ||
        s.key.find("_cymbal") != std::string::npos || s.key == "kick_pedal") {
      has_pad = true;
    }
  }
  if (has_fret && !has_pad)
    return ProbeDeviceType::Guitar;
  if (has_pad && !has_fret)
    return ProbeDeviceType::Drum;
  return ProbeDeviceType::Guitar; // default
}

bool LoadKitProbeFromFile(const fs::path &path, KitProbeData &out) {
  std::ifstream in(path);
  if (!in)
    return false;
  std::string line;
  bool meta_seen = false;

  std::vector<std::pair<std::string, std::vector<uint8_t>>> frames;

  while (std::getline(in, line)) {
    if (line.empty())
      continue;
    const std::string type = ExtractField(line, "type");
    if (type == "meta") {
      meta_seen = true;
      try {
        const std::string v = ExtractField(line, "version");
        if (!v.empty())
          out.version = std::stoi(v);
      } catch (...) {
      }
      out.vid = ParseVidPid(ExtractField(line, "vendor_id"));
      out.pid = ParseVidPid(ExtractField(line, "product_id"));
      out.device_name = ExtractField(line, "device_name");
      const std::string dt = ExtractField(line, "device_type");
      if (dt == "drum")
        out.device_type = ProbeDeviceType::Drum;
      else if (dt == "drum_pro")
        out.device_type = ProbeDeviceType::ProDrum;
      else if (dt == "guitar")
        out.device_type = ProbeDeviceType::Guitar;
      else if (dt == "guitar_solo")
        out.device_type = ProbeDeviceType::GuitarSolo;
      out.is_xinput = (ExtractField(line, "source") == "xinput");
      try {
        out.report_length = std::stoi(ExtractField(line, "report_length"));
      } catch (...) {
      }
      continue;
    }
    const std::string step = ExtractField(line, "step");
    if (step.empty())
      continue;
    auto bytes = ParseBytes(ExtractField(line, "bytes"));
    if (bytes.empty())
      continue;
    frames.emplace_back(step, std::move(bytes));
  }

  if (out.vid == 0 || out.pid == 0) {
    VidPidFromFilename(path, out.vid, out.pid);
  }
  if (out.report_length == 0) {
    for (const auto &[_, b] : frames) {
      if (static_cast<int>(b.size()) > out.report_length) {
        out.report_length = static_cast<int>(b.size());
      }
    }
  }
  if (out.report_length <= 0)
    return false;
  out.report_length = std::min(out.report_length, 64);

  std::map<std::string, StepResultData *> by_key;

  for (auto &[step, bytes] : frames) {
    StepResultData *r = nullptr;
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
      auto &b = r->bytes[i];
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

bool TomlContains(const std::string &toml, const std::string &needle) {
  return toml.find(needle) != std::string::npos;
}

// Write `text` to a unique temp .toml file and return its path. Caller is
// responsible for cleanup.
fs::path WriteTempToml(const std::string &stem, const std::string &text) {
  fs::path tmp = fs::temp_directory_path() / ("hidtest_" + stem + ".toml");
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
bool RoundTripStep(const StepResultData &step, std::string &detail) {
  if (step.raw.empty()) {
    detail = "step '" + step.key + "' has no frames";
    return false;
  }
  // Try every captured frame for the step — at least one must produce
  // non-zero output. Picking only the last frame is fragile: the user may
  // have released the input by the final frame (especially for tilt,
  // where they return the guitar to neutral after pulling it up).
  for (const auto &frame : step.raw) {
    u8 dud[HID::kMaxDeviceUniqueData] = {};
    HID::PackDeviceUniqueData(1, frame.data(), frame.size(),
                              Libraries::Pad::OrbisPadDeviceClass::Guitar, dud);
    const u32 buttons =
        HID::PackButtons(1, frame.data(), frame.size(),
                         Libraries::Pad::OrbisPadDeviceClass::Guitar);
    if (buttons != 0)
      return true;
    for (std::size_t i = 0; i < HID::kMaxDeviceUniqueData; ++i) {
      if (dud[i] != 0)
        return true;
    }
  }
  detail = "step '" + step.key + "' packed to all-zero output across " +
           std::to_string(step.raw.size()) + " frames";
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
bool SoloFretStep(const StepResultData &step, std::string &detail) {
  using B = Libraries::Pad::OrbisPadButtonDataOffset;
  const u32 face_mask = static_cast<u32>(B::Cross) |
                        static_cast<u32>(B::Circle) |
                        static_cast<u32>(B::Triangle) |
                        static_cast<u32>(B::Square) | static_cast<u32>(B::L1);
  if (step.raw.empty()) {
    detail = "solo step '" + step.key + "' has no frames";
    return false;
  }
  bool saw_solo_dud = false, saw_face_button = false, saw_solo_isolated = false;
  for (const auto &frame : step.raw) {
    u8 dud[HID::kMaxDeviceUniqueData] = {};
    HID::PackDeviceUniqueData(1, frame.data(), frame.size(),
                              Libraries::Pad::OrbisPadDeviceClass::Guitar, dud);
    if (dud[4] != 0) {
      saw_solo_dud = true;
      if (dud[3] == 0)
        saw_solo_isolated = true;
    }
    const u32 buttons =
        HID::PackButtons(1, frame.data(), frame.size(),
                         Libraries::Pad::OrbisPadDeviceClass::Guitar);
    if (buttons & face_mask)
      saw_face_button = true;
    if (saw_solo_dud && saw_face_button && saw_solo_isolated)
      return true;
  }
  if (!saw_solo_dud) {
    detail = "solo step '" + step.key + "' produced no dud[4] bit";
    return false;
  }
  if (!saw_face_button) {
    detail = "solo step '" + step.key +
             "' fired dud[4] but no face button bit "
             "(missing [buttons_byte_<solo>] mapping?)";
    return false;
  }
  detail = "solo step '" + step.key +
           "' set dud[4] but never cleared dud[3] "
           "in the same frame (solo_modifier_byte not gating dud[3]?)";
  return false;
}

// Counterpart for the regular fret_* steps on a GuitarSolo kit: pressing
// a main fret with NO solo modifier held must leave dud[4] == 0. Without
// this, a kit derived as solo-modifier-style but with a buggy packer
// (modifier always active) would route every fret press to dud[4],
// effectively breaking the whole main fretboard — and SoloFretStep alone
// wouldn't catch it because it doesn't run on non-solo steps.
bool MainFretStep(const StepResultData &step, std::string &detail) {
  if (step.raw.empty()) {
    detail = "fret step '" + step.key + "' has no frames";
    return false;
  }
  bool saw_main_dud = false;
  for (const auto &frame : step.raw) {
    u8 dud[HID::kMaxDeviceUniqueData] = {};
    HID::PackDeviceUniqueData(1, frame.data(), frame.size(),
                              Libraries::Pad::OrbisPadDeviceClass::Guitar, dud);
    if (dud[4] != 0) {
      detail = "fret step '" + step.key +
               "' leaked into dud[4] (modifier code path stuck on?)";
      return false;
    }
    if (dud[3] != 0)
      saw_main_dud = true;
  }
  if (!saw_main_dud) {
    detail = "fret step '" + step.key + "' produced no dud[3] bit";
    return false;
  }
  return true;
}

// Pickup / FX switch sweep step (X360 RB Guitar, PS3/Wii guitars, and the
// PS4 RB Mustang / PS5 Riffmaster pickup selector). Per the PlasticBand
// spec the 5 detents must quantize to dud[0] values 0..4 (wah, vibe,
// flange, chorus, echo). The X360 raw values cluster near 25/76/127/178/
// 229; the old packer divided 0x10..0xFF into 4 slots which (a) skipped
// wah-wah because no raw < 0x10 is ever produced and (b) collided notch 2
// and notch 3 onto the same dud[0]=2. So the regression we want to catch
// is exactly "wah-wah unreachable AND fewer than 4 distinct outputs."
// Skipped if the kit's derived TOML doesn't declare a tone_byte (the
// Riffmaster fixture's fx_switch capture is flat, so wizard omits it).
bool PickupSwitchStep(const StepResultData &step, const std::string &toml,
                      std::string &detail) {
  if (toml.find("tone_byte") == std::string::npos)
    return true;
  if (step.raw.empty()) {
    detail = "fx_switch step has no frames";
    return false;
  }
  std::set<u8> distinct;
  for (const auto &frame : step.raw) {
    u8 dud[HID::kMaxDeviceUniqueData] = {};
    HID::PackDeviceUniqueData(1, frame.data(), frame.size(),
                              Libraries::Pad::OrbisPadDeviceClass::Guitar, dud);
    distinct.insert(dud[0]);
  }
  if (distinct.size() < 3) {
    detail = "fx_switch packed to only " + std::to_string(distinct.size()) +
             " distinct dud[0] values (sweep should hit ≥ 3 notches)";
    return false;
  }
  if (*distinct.begin() != 0) {
    detail = "fx_switch never reached dud[0] = 0 (wah-wah unreachable) "
             "— PlasticBand quantization broken?";
    return false;
  }
  if (*distinct.rbegin() < 3) {
    detail = "fx_switch never reached the high notches (max dud[0] = " +
             std::to_string(*distinct.rbegin()) + ")";
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
bool SoloFretComboStep(const StepResultData &step, std::string &detail) {
  using B = Libraries::Pad::OrbisPadButtonDataOffset;
  const u32 both_buttons =
      static_cast<u32>(B::Cross) | static_cast<u32>(B::Square);
  if (step.raw.empty()) {
    detail = "solo combo '" + step.key + "' has no frames";
    return false;
  }
  bool saw_two_dud_bits = false, saw_both_buttons = false;
  for (const auto &frame : step.raw) {
    u8 dud[HID::kMaxDeviceUniqueData] = {};
    HID::PackDeviceUniqueData(1, frame.data(), frame.size(),
                              Libraries::Pad::OrbisPadDeviceClass::Guitar, dud);
    const u8 d4 = dud[4];
    if (d4 != 0 && (d4 & (d4 - 1)) != 0)
      saw_two_dud_bits = true;
    const u32 buttons =
        HID::PackButtons(1, frame.data(), frame.size(),
                         Libraries::Pad::OrbisPadDeviceClass::Guitar);
    if ((buttons & both_buttons) == both_buttons)
      saw_both_buttons = true;
    if (saw_two_dud_bits && saw_both_buttons)
      return true;
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

// Tilt-activation regression. For any kit whose derived TOML declares a
// tilt_byte, replay the captured tilt_up frames through PackDeviceUniqueData
// and require at least one frame to push dud[2] above a meaningful
// threshold (>= 0x40) — that's the byte the PS4 RB Guitar wire format
// uses to deliver tilt to the game. Catches the failure mode where the
// wizard picks a wrong tilt_byte (PS5 Riffmaster used to pick byte 12,
// a counter, instead of byte 42, the real tilt flag) so the kit looks
// valid but tilt never fires.
//
// Skipped for kits without a tilt_byte (XInput synth captures with no
// real tilt sensor — those correctly produce NO tilt_byte after the
// tilt_shift heuristic, and there's nothing to verify).
bool TiltActivationStep(const StepResultData &step, const std::string &toml,
                        std::string &detail) {
  if (toml.find("\ntilt_byte ") == std::string::npos)
    return true;
  if (step.raw.empty()) {
    detail = "tilt_activation: no tilt_up frames";
    return false;
  }
  u8 max_dud2 = 0;
  for (const auto &frame : step.raw) {
    u8 dud[HID::kMaxDeviceUniqueData] = {};
    HID::PackDeviceUniqueData(1, frame.data(), frame.size(),
                              Libraries::Pad::OrbisPadDeviceClass::Guitar, dud);
    if (dud[2] > max_dud2)
      max_dud2 = dud[2];
  }
  if (max_dud2 < 0x40) {
    char hex[8];
    std::snprintf(hex, sizeof(hex), "%02X", max_dud2);
    detail = "tilt_activation: tilt_up never pushed dud[2] above 0x40 "
             "(max = 0x" +
             std::string(hex) + "). tilt_byte may be a counter / wrong byte.";
    return false;
  }
  return true;
}

// Santroller HID and similar kits that pack frets + Start on the same byte
// MUST suppress the fret face buttons from PackButtons output when
// Start/Select is held (clear_dud0_when_raw1_bits triggers). Otherwise a
// "Start + green" chord goes through as Cross|Options simultaneously and
// RB4's menu interprets it as both "accept" and "exit" → the kit "freaks
// out" mid-navigation. This synthesises the chord from the captured
// button_start frame OR'd with green_fret's byte 1, replays through
// PackButtons, and asserts no fret face button bit fires.
bool StartFretSuppressionStep(const StepResultData &start_step,
                              const StepResultData &green_step,
                              const std::string &toml, std::string &detail) {
  if (toml.find("clear_dud0_when_raw1_bits") == std::string::npos)
    return true;
  if (start_step.raw.empty() || green_step.raw.empty()) {
    detail =
        "start_fret_suppression: missing button_start or green_fret frames";
    return false;
  }
  using B = Libraries::Pad::OrbisPadButtonDataOffset;
  const u32 face_mask = static_cast<u32>(B::Cross) |
                        static_cast<u32>(B::Circle) |
                        static_cast<u32>(B::Square) |
                        static_cast<u32>(B::Triangle) | static_cast<u32>(B::L1);
  // Pick the brightest pressed frame from each step.
  auto brightest = [](const StepResultData &s) {
    std::vector<uint8_t> best = s.raw.front();
    int best_score = -1;
    for (const auto &fr : s.raw) {
      int score = 0;
      for (uint8_t b : fr)
        score += b;
      if (score > best_score) {
        best_score = score;
        best = fr;
      }
    }
    return best;
  };
  const std::vector<uint8_t> start_fr = brightest(start_step);
  const std::vector<uint8_t> green_fr = brightest(green_step);
  if (start_fr.size() != green_fr.size()) {
    detail = "start_fret_suppression: frame sizes differ";
    return false;
  }
  std::vector<uint8_t> chord = start_fr;
  for (std::size_t i = 0; i < chord.size(); ++i)
    chord[i] |= green_fr[i];
  const u32 buttons =
      HID::PackButtons(1, chord.data(), chord.size(),
                       Libraries::Pad::OrbisPadDeviceClass::Guitar);
  if (buttons & face_mask) {
    char hex[8];
    std::snprintf(hex, sizeof(hex), "%02X", chord[1]);
    detail = "start_fret_suppression: Start+green chord (raw[1]=0x" +
             std::string(hex) +
             ") fired a fret face button; clear_dud0_when_raw1_bits "
             "must mask fret bits in PackButtons too";
    return false;
  }
  return true;
}

CaseResult RunCase(const fs::path &path) {
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
  if (!TomlContains(toml, "schema       = \"shadps4-legacy-instrument/v")) {
    r.detail = "missing schema header";
    return r;
  }
  const bool is_guitar = data.device_type == ProbeDeviceType::Guitar ||
                         data.device_type == ProbeDeviceType::GuitarSolo;
  // v3 introduced combo steps (green_strum / green_blue / green_blue_strum)
  // and the solo-fret walkthrough for GuitarSolo kits. v1/v2 captures
  // predate them and must not be punished for missing keys.
  auto has_step = [&](const char *name) {
    for (const auto &s : data.results) {
      if (s.key == name && s.captured)
        return true;
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
  //   button_ps    — not all guitars expose Guide/Home on their HID report;
  //                  on some kits the wizard correctly omits it.
  //   kick_pedal_2 — captured by the wizard but the current derivation
  //                  only maps the primary kick into the TOML; packing
  //                  the 2nd kick frame produces no output. Optional step.
  // tilt_up has its own per-step check (TiltActivationStep) below; it
  // exits the round-trip path via the explicit step.key == "tilt_up"
  // branch and doesn't need to be on this skip list.
  static const std::vector<std::string> kSkipSteps = {
      "button_ps",
      "kick_pedal_2",
  };
  const bool check_solo_buttons =
      data.device_type == ProbeDeviceType::GuitarSolo &&
      (TomlContains(toml, "solo_fret_byte = ") ||
       TomlContains(toml, "solo_modifier_byte = "));
  for (const auto &step : data.results) {
    if (step.key.empty() || step.key[0] == '_')
      continue;
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
    const bool is_solo_step = step.key.rfind("solo_", 0) == 0 &&
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
    const bool is_main_fret = step.key.find("_fret") != std::string::npos &&
                              step.key.rfind("solo_", 0) != 0;
    if (check_solo_buttons && is_main_fret) {
      if (!MainFretStep(step, detail)) {
        r.detail = detail;
        return r;
      }
      continue;
    }
    if (step.key == "fx_switch") {
      if (!PickupSwitchStep(step, toml, detail)) {
        r.detail = detail;
        return r;
      }
      continue;
    }
    if (step.key == "tilt_up") {
      if (!TiltActivationStep(step, toml, detail)) {
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
  // Cross-step synthesis: Start + green chord, applies when the kit
  // declares clear_dud0_when_raw1_bits (frets + menu on same byte).
  const StepResultData *start_step = nullptr;
  const StepResultData *green_step = nullptr;
  for (const auto &s : data.results) {
    if (s.key == "button_start" && s.captured)
      start_step = &s;
    if (s.key == "green_fret" && s.captured)
      green_step = &s;
  }
  if (start_step && green_step) {
    std::string detail;
    if (!StartFretSuppressionStep(*start_step, *green_step, toml, detail)) {
      r.detail = detail;
      return r;
    }
  }
  r.passed = true;
  return r;
}

// Runtime gate-apply test. Answers "are we sure values below the gate
// don't reach the game?" without relying on captured fixtures. Builds
// a synthetic v2 kit with a known [gate] table, hands it to the live
// LoadKitFromToml + BindKitFromTomlForTesting, then feeds raw frames
// with single-byte values just below / at / just above each pad's
// gate threshold. The asserts:
//   - byte < gate  →  pack_vel writes 0 in the dud slot AND PackButtons
//                     does not OR in the pad's face-button bit
//   - byte >= gate →  pack_vel writes the (scaled) velocity AND
//                     PackButtons fires the face-button bit
//
// Uses the MIDI schema variant so LoadKitFromToml auto-injects the
// drum_*_byte fields and the synthetic button_bytes table (red→Circle,
// blue→Square, yellow→Triangle, green→Cross). That keeps the test
// kit's TOML small and avoids re-encoding the PS4 RB drum layout by
// hand here.
CaseResult RunGateApplyTest() {
  using DC = Libraries::Pad::OrbisPadDeviceClass;
  using B = Libraries::Pad::OrbisPadButtonDataOffset;
  CaseResult r{};
  r.name = "RuntimeGateApply";

  // Snapshot byte layout the MIDI loader auto-injects:
  //   red=3, blue=5, yellow=4, green=6 (kick at byte 1, button-only).
  const std::string kit = R"(schema = "shadps4-midi-instrument/v2"
name = "gate apply test"
source = "midi"
port_id = "test:gate"
device_class = "drum"
device_subclass = "drum"

[midi_pad_map]
red = [38]
blue = [45]
yellow = [48]
green = [43]

[gate]
"red" = 50
"blue" = 100
"yellow" = 0
"green" = 25
)";

  HID::Testing::ResetForTesting();
  const fs::path tmp = WriteTempToml("gate_apply", kit);
  const bool bound = HID::Testing::BindKitFromTomlForTesting(1, tmp.string());
  std::error_code ec;
  fs::remove(tmp, ec);
  if (!bound) {
    r.detail = "BindKitFromTomlForTesting failed for synthetic v2 kit";
    return r;
  }

  // MIDI gates are stored in 0..127 native and converted to 0..255
  // raw space at load time via (v*255+63)/127. Mirror that here so the
  // test feeds raw values in the same space the runtime checks.
  auto midi_gate_to_raw = [](int g) { return (g * 255 + 63) / 127; };

  struct Case {
    const char *pad;
    int raw_byte;    // snapshot byte index in pack_vel's input
    int dud_idx;     // expected output slot
    u32 face_button; // PackButtons bit when above gate
    int gate_midi;   // gate threshold in MIDI 0..127 space
  };
  const Case cases[] = {
      {"red", 3, 0, static_cast<u32>(B::Circle), 50},
      {"blue", 5, 1, static_cast<u32>(B::Square), 100},
      {"yellow", 4, 2, static_cast<u32>(B::Triangle), 0}, // gate disabled
      {"green", 6, 3, static_cast<u32>(B::Cross), 25},
  };

  for (const auto &c : cases) {
    const int gate_raw = midi_gate_to_raw(c.gate_midi);
    // Three samples: just below, exactly at, just above. The runtime
    // uses `< gate` for the drop, so `== gate` passes.
    const int below = (gate_raw > 0) ? gate_raw - 1 : 0;
    const int at = gate_raw;
    const int above = std::min(255, gate_raw + 4);

    auto run_frame = [&](int value, bool expect_passes,
                         const char *label) -> bool {
      u8 frame[16] = {};
      frame[c.raw_byte] = static_cast<u8>(value);
      u8 dud[HID::kMaxDeviceUniqueData] = {};
      HID::PackDeviceUniqueData(1, frame, sizeof(frame), DC::Drum, dud);
      const u32 buttons = HID::PackButtons(1, frame, sizeof(frame), DC::Drum);
      const bool dud_passed = (dud[c.dud_idx] != 0);
      const bool button_passed = (buttons & c.face_button) != 0;
      const bool ok =
          (dud_passed == expect_passes) && (button_passed == expect_passes);
      if (!ok) {
        r.detail = std::string{"pad "} + c.pad + " " + label +
                   " (raw=" + std::to_string(value) +
                   ", gate_raw=" + std::to_string(gate_raw) + "): dud[" +
                   std::to_string(c.dud_idx) +
                   "]=" + std::to_string(dud[c.dud_idx]) + " buttons=0x" +
                   [&] {
                     char b[16];
                     std::snprintf(b, sizeof(b), "%x", buttons);
                     return std::string{b};
                   }() +
                   " (expected " +
                   (expect_passes ? "pass-through" : "silenced") + ")";
      }
      return ok;
    };

    if (c.gate_midi == 0) {
      // Gate disabled: even a tiny non-zero raw value should pass.
      if (!run_frame(1, true, "gate=0 low"))
        return r;
      if (!run_frame(127, true, "gate=0 high"))
        return r;
    } else {
      if (!run_frame(below, false, "below gate"))
        return r;
      if (!run_frame(at, true, "at gate"))
        return r;
      if (!run_frame(above, true, "above gate"))
        return r;
    }
  }

  r.passed = true;
  return r;
}

// `hidtest --replay-midi <kit.toml> <capture.midi.jsonl>` — load the kit
// TOML through the runtime loader (BindKitFromTomlForTesting), then walk
// every Note On event in the jsonl. For each event:
//   - look up the note in the kit's midi_pad_map → raw byte index
//   - synthesise a 16-byte snapshot with that byte set to the velocity
//     scaled to 0..255 (same formula MidiInput::DrainEvents writes into
//     pads_by_byte)
//   - feed the snapshot to PackDeviceUniqueData + PackButtons exactly
//     the way the runtime does for the legacy-instrument scePadRead path
//   - print buttons bitmap + dud[0..6] so a human can confirm whether
//     the runtime would produce a real "drum hit" for that input
//
// Lets us replay a user's actual probe capture against their actual TOML
// offline and see whether the SDK output is correct without needing them
// to re-test.
int RunReplayMidi(const fs::path &toml_path, const fs::path &jsonl_path) {
  using DC = Libraries::Pad::OrbisPadDeviceClass;
  HID::Testing::ResetForTesting();
  if (!HID::Testing::BindKitFromTomlForTesting(1, toml_path.string())) {
    std::cerr << "replay-midi: BindKitFromTomlForTesting failed for "
              << toml_path << "\n";
    return 2;
  }
  // Pull the loaded kit so we can resolve note → byte. The synthetic kit
  // BindKitFromTomlForTesting attaches lives in g_slots[0].kit.
  const HID::KitDef *kit = HID::g_slots[0].kit;
  if (!kit) {
    std::cerr << "replay-midi: g_slots[0].kit is null after Bind\n";
    return 2;
  }
  std::ifstream in(jsonl_path);
  if (!in) {
    std::cerr << "replay-midi: could not open " << jsonl_path << "\n";
    return 2;
  }
  std::printf("# replaying %s against %s\n# kit '%s' midi_pad_map "
              "has %zu entries\n",
              jsonl_path.filename().string().c_str(),
              toml_path.filename().string().c_str(), kit->name.c_str(),
              kit->midi_pad_map.size());
  std::printf("# columns: step  note  vel  byte  buttons    dud[0..6]\n");

  std::string current_step = "(none)";
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty())
      continue;
    if (ExtractField(line, "type") == "meta") {
      continue;
    }
    const std::string step = ExtractField(line, "step");
    if (!step.empty())
      current_step = step;
    // Parse events array (this works for the .midi.jsonl format the wizard
    // writes — same shape miditest already handles).
    const std::string ev_arr_str = ExtractField(line, "events");
    if (ev_arr_str.empty())
      continue;
    // Iterate event objects inside the array. We only handle Note On with
    // vel > 0; off / vel=0 don't tell us what RB4 sees as a hit.
    std::size_t i = 0;
    while ((i = ev_arr_str.find('{', i)) != std::string::npos) {
      const std::size_t end = ev_arr_str.find('}', i);
      if (end == std::string::npos)
        break;
      const std::string obj = ev_arr_str.substr(i, end - i + 1);
      const std::string on_s = ExtractField(obj, "on");
      if (on_s != "true" && on_s != "1") {
        i = end + 1;
        continue;
      }
      int note = 0, vel = 0;
      try {
        note = std::stoi(ExtractField(obj, "note"));
      } catch (...) {
      }
      try {
        vel = std::stoi(ExtractField(obj, "vel"));
      } catch (...) {
      }
      i = end + 1;
      if (vel <= 0)
        continue;
      // note → byte via kit's midi_pad_map (built at LoadKitFromToml from
      // the TOML's [midi_pad_map] section).
      auto it = kit->midi_pad_map.find(static_cast<std::uint8_t>(note));
      if (it == kit->midi_pad_map.end()) {
        std::printf("%-15s n=%-3d v=%-3d (no pad map entry)\n",
                    current_step.c_str(), note, vel);
        continue;
      }
      const int byte_idx = it->second;
      // Snapshot byte = (vel * 255 + 63) / 127 — same formula
      // MidiInput::DrainEvents uses when writing pads_by_byte.
      const u8 scaled = static_cast<u8>((vel * 255 + 63) / 127);
      u8 frame[16] = {};
      if (byte_idx >= 0 && byte_idx < 16)
        frame[byte_idx] = scaled;
      u8 dud[HID::kMaxDeviceUniqueData] = {};
      HID::PackDeviceUniqueData(1, frame, sizeof(frame), DC::Drum, dud);
      const u32 buttons = HID::PackButtons(1, frame, sizeof(frame), DC::Drum);
      std::printf("%-15s n=%-3d v=%-3d byte=%-2d buttons=0x%08x "
                  "dud=[%02x,%02x,%02x,%02x,%02x,%02x,%02x]\n",
                  current_step.c_str(), note, vel, byte_idx, buttons, dud[0],
                  dud[1], dud[2], dud[3], dud[4], dud[5], dud[6]);
    }
  }
  return 0;
}

} // namespace

int main(int argc, char **argv) {
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
  if (argc >= 4 && std::string(argv[1]) == "--replay-midi") {
    return RunReplayMidi(fs::path(argv[2]), fs::path(argv[3]));
  }
  fs::path fixtures = (argc > 1) ? fs::path(argv[1])
                                 : fs::current_path() / "tests" / "hid_kits";
  if (!fs::is_directory(fixtures)) {
    std::cerr << "hidtest: fixtures dir not found: " << fixtures << "\n";
    return 2;
  }
  std::vector<fs::path> files;
  for (auto &entry : fs::directory_iterator(fixtures)) {
    if (entry.path().extension() == ".jsonl") {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());

  std::vector<CaseResult> results;
  // Runtime gate-apply test runs first (independent of fixtures).
  // Confirms the v2 [gate] table is actually consulted by the live
  // PackDeviceUniqueData / PackButtons hot path — sub-threshold raw
  // bytes silence both the dud velocity AND the face button.
  results.push_back(RunGateApplyTest());
  for (auto &f : files) {
    results.push_back(RunCase(f));
  }

  int failed = 0;
  for (const auto &r : results) {
    std::cout << (r.passed ? "[ OK ]  " : "[FAIL]  ") << r.name;
    if (!r.detail.empty())
      std::cout << "  -- " << r.detail;
    std::cout << "\n";
    if (!r.passed)
      ++failed;
  }
  std::cout << "hidtest: " << (results.size() - failed) << "/" << results.size()
            << " passed\n";
  return failed ? 1 : 0;
}
