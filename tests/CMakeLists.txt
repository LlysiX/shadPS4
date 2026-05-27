# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

find_package(Python3 COMPONENTS Interpreter)

if(NOT Python3_Interpreter_FOUND)
    message(WARNING "Python3 not found — skipping HID kit regression tests")
    return()
endif()

add_test(
    NAME hid_kit_validation
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/validate_hid_kits.py
            ${CMAKE_CURRENT_SOURCE_DIR}/hid_kits
)
set_tests_properties(hid_kit_validation PROPERTIES
    LABELS "hid"
    TIMEOUT 60
)
