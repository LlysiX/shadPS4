// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Per-player input-device assignment editor. Tabbed UI: one tab per PS4
// player slot (1..4), each tab is a self-contained panel for that slot
// containing:
//   - the device list (gamepads / kits / keyboard / MIDI),
//   - a "Reports as" pad-class picker (auto-overridden by the kit TOML
//     when legacy raw-HID is on for the slot — combo shows "Automatic"),
//   - a Legacy raw-HID toggle + per-slot probe wizard launcher,
//   - the mic device + gate enable + threshold slider.
//
// The 4-slot grid that used to live in Configure Special Devices was
// folded into here so the player profile is configured from one place.
// Configure Special Devices is now a system-wide kit library (probe new
// kit, show loaded kits, install udev rules).
//
// Programmatic UI — no .ui file.

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

    // Open / close the dialog-local SDL recording stream that feeds the
    // VU meter while no game is running. Called on tab build, on mic
    // dropdown changes, and on destruction.
    void openMicPreview(int slot);
    void closeMicPreview(int slot);
    // Close any previously-opened MIDI port for this slot and re-open
    // against the (now-current) MIDI PlayerDevice. Called whenever the
    // device list mutates — adding / removing MIDI rows after the
    // dialog opened wouldn't otherwise drive the tab indicator.
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
        // Live gate-open tracking for the MicVuBar's colour. When the
        // most recent sample is above threshold, the gate is "open"
        // (green fill); otherwise it stays grey while the hold timer
        // is decaying. Carries the timestamp of the last above-
        // threshold sample so the next tick can apply the hold window.
        std::chrono::steady_clock::time_point lastAboveThreshold{};
        bool gateOpen = false;
        // Custom-painted VU meter — declared as a bare QWidget here so
        // the dialog header doesn't need the inline MicVuBar definition.
        // The .cpp casts it back to MicVuBar to set level / threshold.
        QWidget* micVu = nullptr;
        // Dialog-local SDL recording stream. The game's sceAudioIn
        // hasn't run yet while the user is in settings, so we open our
        // own preview stream off the configured mic device — same
        // pattern Settings → Audio uses. Closed and reopened when the
        // user changes the mic dropdown.
        SDL_AudioStream* previewStream = nullptr;
        // Cached user-picked class so the combo can restore it when
        // legacy is toggled off (legacy mode hijacks the display to
        // "Automatic" while it's on).
        int lastUserClass = 0;
    };
    std::array<SlotWidgets, 4> m_slots{};
    QTabWidget* m_tabs = nullptr;

    // Live gamepad polling so the tab indicator works even when no game
    // window is open (the runtime FinalizeUpdate path only fires while
    // a game is loaded). The dialog opens one SDL_Gamepad handle per
    // attached pad, polls button state at the same 50 ms tick, and
    // bumps the slot's input timestamp on any state change. Keyboard
    // input is caught via an event filter on the dialog itself.
    std::unordered_map<SDL_JoystickID, SDL_Gamepad*> m_polled_pads;
    // Track keyboard-held state via the dialog's event filter so the
    // "Keyboard" rows can glow while a key is down. Reset on KeyRelease.
    bool m_keyboard_held = false;
    // Per-slot dialog-local MIDI port handle. Opened against whatever
    // PlayerDevice MIDI entry sits in the slot at dialog construction
    // so the tab indicator (and row glow) light up when the user taps
    // their MIDI drum while configuring. void* is the opaque type the
    // MidiInput layer hands back to avoid pulling its header here.
    std::array<void*, 4> m_polled_midi{};
    // Per-slot timestamp of the last MIDI event observed in the
    // dialog's drain — drives the "MIDI" row glow even when there is
    // no probed kit attached yet.
    std::array<std::chrono::steady_clock::time_point, 4> m_last_midi_ns{};

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
};
