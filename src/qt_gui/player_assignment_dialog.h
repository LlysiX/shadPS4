// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Per-player input-device assignment editor. Lets the user pin specific
// SDL gamepads, raw-HID kits, the virtual keyboard, or MIDI input ports
// to specific PS4 player slots (1..4) instead of accepting the default
// first-come-first-served placement. Backed by Config::PlayerDevice
// records read/written via Config::get/setPlayerSlotDevices.
//
// Programmatic UI — no .ui file. Four QListWidget sections (one per
// slot) with Add / Remove buttons and an OK/Cancel pair at the bottom.

#pragma once

#include <array>
#include <QDialog>

class QListWidget;

class PlayerAssignmentDialog : public QDialog {
    Q_OBJECT
public:
    explicit PlayerAssignmentDialog(QWidget* parent = nullptr);

private:
    void refreshSlotList(int slot);
    void addGamepadDevice(int slot);
    void addKitDevice(int slot);
    void addKeyboardDevice(int slot);
    void addMidiDevice(int slot);
    void removeSelectedDevice(int slot);
    void onAccept();

    std::array<QListWidget*, 4> m_lists{};
};
