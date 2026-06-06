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

#include <QDialog>
#include <QString>
#include <vector>

class QListWidget;
class QListWidgetItem;

class DevicePickerDialog : public QDialog {
    Q_OBJECT
public:
    DevicePickerDialog(const QString& title, const QString& help_text,
                       QWidget* parent = nullptr);

    // Append one row. label is what the user sees. encoded is whatever
    // string the caller wants to map back to the chosen device (the
    // Config::encodePlayerDevice output is the obvious one). When
    // enabled is false the row renders greyed out and the OK button
    // stays disabled while it's selected. disabled_hint, if non-empty,
    // appears as a tooltip on the row.
    void addRow(const QString& label, const QString& encoded, bool enabled,
                const QString& disabled_hint = QString());

    // After exec(), returns the encoded string of the chosen row. Empty
    // when the dialog was cancelled or no enabled row was selected.
    QString chosenEncoded() const { return m_chosen; }

private slots:
    void onSelectionChanged();
    void onItemActivated(QListWidgetItem* item);
    void onAccept();

private:
    QListWidget* m_list;
    QString m_chosen;
    class QPushButton* m_ok;
};
