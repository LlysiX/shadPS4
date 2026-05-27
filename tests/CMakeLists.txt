# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# HID kit regression harness. Links against the same DeriveKitToml the
# in-app wizard uses AND the same hid_packer.cpp the emulator runs at play
# time, so each fixture is driven through the full pipeline:
#   raw JSONL frames -> KitProbeData -> DeriveKitToml -> TOML
#                                  -> LoadKitFromToml -> PackButtons /
#                                  PackDeviceUniqueData -> assertions.
# Deliberately not linked against SDL3; hid_packer.cpp is SDL-free (the IO
# layer in hid_instrument.cpp owns gamepad/hidraw handles) so this exe
# builds and runs on every platform without a real device.

add_executable(hidtest
    hidtest/main.cpp
    hidtest/stubs.cpp
    ${CMAKE_SOURCE_DIR}/src/input/hid_kit_probe_derive.cpp
    ${CMAKE_SOURCE_DIR}/src/input/hid_packer.cpp
)

target_include_directories(hidtest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(hidtest PRIVATE cxx_std_23)
target_link_libraries(hidtest PRIVATE fmt::fmt toml11::toml11)

add_test(
    NAME hid_kit_validation
    COMMAND hidtest ${CMAKE_CURRENT_SOURCE_DIR}/hid_kits
)
set_tests_properties(hid_kit_validation PROPERTIES
    LABELS "hid"
    TIMEOUT 60
)

# Synthetic fixtures generated from the PlasticBand specs (no real
# hardware). One per kit class shadPS4 claims to support, so the packer
# round-trip is exercised even when the project has no community capture
# for that device yet.
add_test(
    NAME hid_kit_synthetic
    COMMAND hidtest ${CMAKE_CURRENT_SOURCE_DIR}/hid_kits/synthetic
)
set_tests_properties(hid_kit_synthetic PROPERTIES
    LABELS "hid"
    TIMEOUT 60
)
