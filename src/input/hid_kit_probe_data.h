// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Non-Qt mirror of the wizard's in-memory probe state. The wizard fills one
// of these from its captured StepResults; the test harness fills the same
// shape from a .raw.jsonl file. DeriveKitToml takes a KitProbeData and emits
// the TOML the runtime kit loader reads — same function in both paths.

#pragma once

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace Input::HidInstrument {

constexpr int kProbeMaxReportLen = 64;

struct ByteObs {
    int min = 0xFF;
    int max = 0;
    int min_nonzero = -1;
    int transitions = 0;
    int samples = 0;
};

struct StepResultData {
    std::string key;       // e.g. "green_fret"
    std::string kind;      // "digital" / "velocity" / "motion" / "combo" / "tilt_dir"
    bool captured = false;
    std::array<ByteObs, kProbeMaxReportLen> bytes{};
    std::vector<std::vector<uint8_t>> raw;
};

enum class ProbeDeviceType {
    Drum,
    ProDrum,
    Guitar,
    GuitarSolo,  // 5-fret guitar with upper-neck solo frets (PS4/PS5 RB)
};

struct KitProbeData {
    // Raw-jsonl schema version. v1 (default for legacy captures with no
    // version key): basic guitar/drum step list, no combos. v2: added
    // version field. v3: GuitarSteps gained combo steps (green_strum,
    // green_blue, green_blue_strum); added GuitarSolo device type with
    // solo_fret_* steps; DrumSteps gained orange_pad. v4: added the
    // _idle_baseline step (used to set baseline_min/max separately from
    // the motion-sensor sweep). v5: GuitarSolo gained solo_green_blue
    // (two-solo-fret combo) and fx_switch (pickup/FX position sweep).
    // Tests use the version to know which steps MUST be present.
    int version = 1;
    uint16_t vid = 0;
    uint16_t pid = 0;
    std::string device_name;
    ProbeDeviceType device_type = ProbeDeviceType::Guitar;
    bool is_xinput = false;
    int report_length = 0;
    std::array<int, kProbeMaxReportLen> baseline_max{};
    std::array<int, kProbeMaxReportLen> baseline_min{};
    std::set<int> motion_bytes;
    std::vector<StepResultData> results;
};

// Derive a runtime TOML kit definition from probe state. Same code path
std::string DeriveKitToml(const KitProbeData& data);

// Compute baseline_max, baseline_min, and motion_bytes from the recorded
// step frames (specifically _idle_baseline and _motion_baseline).
void DeriveBaselineAndMotion(KitProbeData& data);

}  // namespace Input::HidInstrument
