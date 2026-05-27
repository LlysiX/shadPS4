# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# HID kit regression harness — links against the same DeriveKitToml the
# in-app wizard uses, replays every fixture in tests/hid_kits/, and checks
# that the produced TOML has the right structure for the kit's device class.
# The fixture data is captured raw HID JSONL straight from KitProbeDialog;
# the test caught the fret-byte regression that shipped in v0.2.

add_executable(hidtest
    hidtest/main.cpp
    ${CMAKE_SOURCE_DIR}/src/input/hid_kit_probe_derive.cpp
)

target_include_directories(hidtest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(hidtest PRIVATE cxx_std_23)

add_test(
    NAME hid_kit_validation
    COMMAND hidtest ${CMAKE_CURRENT_SOURCE_DIR}/hid_kits
)
set_tests_properties(hid_kit_validation PROPERTIES
    LABELS "hid"
    TIMEOUT 60
)
