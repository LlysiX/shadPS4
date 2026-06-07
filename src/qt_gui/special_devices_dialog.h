// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Per-slot configuration for "special" PS3/PS4 instruments (guitars, drum
// kits, etc.) and the optional legacy raw-HID passthrough used by RB4 with
// PS3 instruments behind the legacy adapter. Opened from a button in
// Control Settings to avoid cluttering that main dialog.

#pragma once

#include <memory>
#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;

namespace Ui {
class SpecialDevicesDialog;
}

class SpecialDevicesDialog : public QDialog {
    Q_OBJECT
public:
    explicit SpecialDevicesDialog(QWidget* parent = nullptr);
    ~SpecialDevicesDialog() override;

private slots:
    void onApply();
    void onAccept();
    void onInstallUdev();
    void onProbeKit(int slot);
    void onLegacyToggled(int slot, bool checked);
    void onRefresh();

private:
    void loadFromConfig();
    void writeConfig();
    void refreshUdevState();
    void refreshKitStatus();
    void updateLegacyRowVisuals(int slot);
    bool udevRulesInstalled() const;

    std::unique_ptr<Ui::SpecialDevicesDialog> ui;

    // Per-slot widget accessors keep the .cpp readable.
    QCheckBox* useSpecialPadCb(int slot) const;
    QComboBox* specialPadClassCb(int slot) const;
    QCheckBox* legacyCb(int slot) const;
    QLabel* statusLbl(int slot) const;
    QPushButton* probeBtn(int slot) const;
    QLabel* udevWarn(int slot) const;

    // Kit-library section. Built programmatically into the existing
    // root layout (the .ui file's per-player grid is hidden). One row
    // per detected HID VID:PID — green ✓ when probed (a kit TOML
    // matches), grey "not probed" otherwise, with an inline "Probe…"
    // button that opens the same wizard the per-player tab uses.
    void rebuildKitLibrary();
    QListWidget* m_kit_list = nullptr;
    QPushButton* m_probe_btn = nullptr;
};
