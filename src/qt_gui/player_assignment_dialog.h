// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
#pragma once

#include <array>
#include <unordered_map>
#include <QDialog>
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_joystick.h>

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class QSlider;
class QTabWidget;
struct SDL_Gamepad;

class PlayerAssignmentDialog : public QDialog {
    Q_OBJECT
public:
    explicit PlayerAssignmentDialog(QWidget* parent = nullptr);
    ~PlayerAssignmentDialog() override;

private:
    void buildSlotTab(int slot);
    void refreshSlotList(int slot);
    void refreshClassRow(int slot);
    void refreshLegacyVisuals(int slot);

    void addGamepadDevice(int slot);
    void addKitDevice(int slot);
    void addKeyboardDevice(int slot);
    void addMidiDevice(int slot);
    void removeSelectedDevice(int slot);
    void launchProbeWizard(int slot);

    void openMicPreview(int slot);
    void closeMicPreview(int slot);
    // Re-open the MIDI port for a slot whenever its device list mutates —
    // otherwise adding/removing MIDI rows after dialog open doesn't drive
    // the tab indicator.
    void refreshPolledMidi(int slot);

    void onAccept();

    struct SlotWidgets {
        QListWidget* list = nullptr;
        QComboBox* classCombo = nullptr;
        QCheckBox* legacyCb = nullptr;
        QPushButton* probeBtn = nullptr;
        QLabel* udevWarn = nullptr;
        QComboBox* micCombo = nullptr;
        QCheckBox* micGateCb = nullptr;
        QSlider* micGateDb = nullptr;
        QLabel* micGateDbLabel = nullptr;
        QSlider* micGateHoldMs = nullptr;
        QLabel* micGateHoldLabel = nullptr;
        std::chrono::steady_clock::time_point lastAboveThreshold{};
        bool gateOpen = false;
        // Declared as QWidget* so the header doesn't need the inline MicVuBar
        // definition; the .cpp casts it back to MicVuBar to set level/threshold.
        QWidget* micVu = nullptr;
        SDL_AudioStream* previewStream = nullptr;
        // Cached user-picked class so the combo can restore it when the
        // "Automatic" sentinel is removed (legacy off).
        int lastUserClass = 0;
    };
    std::array<SlotWidgets, 4> m_slots{};
    QTabWidget* m_tabs = nullptr;

    // One SDL_Gamepad per attached pad — polled at 50 Hz so the tab
    // indicator works even without a running game (FinalizeUpdate only
    // fires in-game).
    std::unordered_map<SDL_JoystickID, SDL_Gamepad*> m_polled_pads;
    bool m_keyboard_held = false;
    // void* is the opaque type MidiInput hands back — avoids pulling its
    // header into the dialog header.
    std::array<void*, 4> m_polled_midi{};
    std::array<std::chrono::steady_clock::time_point, 4> m_last_midi_ns{};

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
};
