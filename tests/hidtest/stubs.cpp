// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Minimal stubs of common/* symbols needed to link hid_packer.cpp into the
// test exe without dragging in the rest of the emulator (config loader,
// spdlog, the user-dir resolver, etc.). The test never touches the
// emulator state these would otherwise own.

#include <filesystem>
#include <string>

#include "common/config.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "input/hid_instrument.h"

// EnsureInit lives in hid_instrument.cpp which we don't link. Provide a
// no-op so GetLatestReport / GetLatestAcceleration in hid_packer.cpp link.
namespace Input::HidInstrument {
bool EnsureInit() { return true; }
}  // namespace Input::HidInstrument

namespace Config {

bool getSpecialPadLegacyPassUSBRawHID(int /*slot*/) {
    // Tests always want the packer to consider the kit "enabled".
    return true;
}

}  // namespace Config

namespace Common::Log {

void FmtLogMessageImpl(Class /*log_class*/, Level /*log_level*/,
                       const char* /*filename*/, unsigned int /*line_num*/,
                       const char* /*function*/, const char* /*format*/,
                       const fmt::format_args& /*args*/) {
    // Silent — the test prints its own [OK]/[FAIL] lines.
}

}  // namespace Common::Log

namespace Common::FS {

const std::filesystem::path& GetUserPath(PathType /*user_path*/) {
    static const std::filesystem::path empty;
    return empty;
}

std::string GetUserPathString(PathType /*user_path*/) {
    return {};
}

}  // namespace Common::FS
