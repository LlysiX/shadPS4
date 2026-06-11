// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Small list-based picker dialog used by PlayerAssignmentDialog to let
// the user choose a detected input device. Replaces QInputDialog::getItem
// for the picker UIs because:
//   1. QInputDialog::getItem is a dropdown — disabled / "needs probing"
//      rows can't be visually distinguished there.
//   2. The list form gives us a place to hook a live-highlight feature
//      (the picker flashes the row matching whichever device is firing
//      events right now) in a future commit.
// Programmatic UI — no .ui file.

#pragma once

#include <unordered_map>
#include <vector>
#include <QDialog>
#include <QString>
#include <SDL3/SDL_joystick.h>

class QListWidget;
class QListWidgetItem;
class QTimer;
struct SDL_Gamepad;

class DevicePickerDialog : public QDialog {
    Q_OBJECT
public:
    DevicePickerDialog(const QString& title, const QString& help_text, QWidget* parent = nullptr);
    ~DevicePickerDialog() override;

    // Append one row. label is what the user sees. encoded is whatever
    // string the caller wants to map back to the chosen device (the
    // Config::encodePlayerDevice output is the obvious one). When
    // enabled is false the row renders greyed out and the OK button
    // stays disabled while it's selected. disabled_hint, if non-empty,
    // appears as a tooltip on the row. When joystick_id is non-zero,
    // the picker subscribes to that SDL gamepad's button state and
    // flashes the row green whenever any standard button on it is
    // pressed — lets the user identify which physical controller a
    // row corresponds to by tapping a button.
    void addRow(const QString& label, const QString& encoded, bool enabled,
                const QString& disabled_hint = QString(), SDL_JoystickID joystick_id = 0);

    // After exec(), returns the encoded string of the chosen row. Empty
    // when the dialog was cancelled or no enabled row was selected.
    QString chosenEncoded() const {
        return m_chosen;
    }

private slots:
    void onSelectionChanged();
    void onItemActivated(QListWidgetItem* item);
    void onAccept();
    void onPollInputs();

private:
    QListWidget* m_list;
    QString m_chosen;
    class QPushButton* m_ok;
    QTimer* m_poll_timer = nullptr;
    // For each row that registered a joystick id, we keep an open
    // SDL_Gamepad* + the row index. SDL3 permits multiple opens of the
    // same physical device, so this doesn't fight the running
    // GameControllers singleton. The handles are closed in the dtor.
    struct GamepadRow {
        int row;
        SDL_Gamepad* gp;
        bool was_lit = false;
    };
    std::unordered_map<SDL_JoystickID, GamepadRow> m_gamepad_rows;
};
