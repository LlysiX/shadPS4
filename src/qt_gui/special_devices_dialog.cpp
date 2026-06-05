// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "special_devices_dialog.h"
#include "ui_special_devices_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QTimer>

#include <filesystem>

#include "common/config.h"
#include "common/path_util.h"
#include "input/hid_instrument.h"
#include "kit_probe_dialog.h"
#include "player_assignment_dialog.h"

namespace {

// PS4 device class enum, see core/libraries/pad/pad.h::OrbisPadDeviceClass.
constexpr struct {
    int value;
    const char* label;
} kSpecialPadClasses[] = {
    {0, "Standard"},   {1, "Guitar"},     {2, "Drum"},        {3, "DJ Turntable"},
    {4, "Dancemat"},   {5, "Navigation"}, {6, "SteeringWheel"}, {7, "Stick"},
    {8, "FightStick"}, {9, "Gun"},
};

constexpr const char* kUdevRulesPath = "/etc/udev/rules.d/99-shadps4-instruments.rules";

}  // namespace

SpecialDevicesDialog::SpecialDevicesDialog(QWidget* parent)
    : QDialog(parent), ui(new Ui::SpecialDevicesDialog) {
    ui->setupUi(this);

    // Populate the class dropdowns for all four slots.
    for (int slot = 1; slot <= 4; ++slot) {
        auto* cls = specialPadClassCb(slot);
        for (const auto& item : kSpecialPadClasses) {
            cls->addItem(QString::fromUtf8(item.label), item.value);
        }
        // Per-slot probe button: opens the wizard.
        connect(probeBtn(slot), &QPushButton::clicked, this,
                [this, slot]() { onProbeKit(slot); });
        // Per-slot legacy toggle drives the per-row probe/warning visibility.
        connect(legacyCb(slot), &QCheckBox::toggled, this,
                [this, slot](bool checked) { onLegacyToggled(slot, checked); });
    }

    connect(ui->installUdevBtn, &QPushButton::clicked, this,
            &SpecialDevicesDialog::onInstallUdev);
#ifndef __linux__
    // Other platforms grant HID access by default (macOS) or use a different
    // mechanism (Windows). The Linux-flavoured udev installer is meaningless
    // there, so hide it.
    ui->installUdevBtn->setVisible(false);
#endif
    connect(ui->refreshBtn, &QPushButton::clicked, this,
            &SpecialDevicesDialog::onRefresh);
    connect(ui->playerAssignmentBtn, &QPushButton::clicked, this,
            [this]() {
                PlayerAssignmentDialog dlg(this);
                dlg.exec();
            });
    connect(ui->buttonBox, &QDialogButtonBox::clicked, this,
            [this](QAbstractButton* b) {
                const auto role = ui->buttonBox->standardButton(b);
                if (role == QDialogButtonBox::Apply) {
                    onApply();
                } else if (role == QDialogButtonBox::Save) {
                    onAccept();
                }
            });
    connect(ui->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    loadFromConfig();
    refreshUdevState();
    refreshKitStatus();
}

SpecialDevicesDialog::~SpecialDevicesDialog() = default;

// ----- widget accessors -------------------------------------------------------
QCheckBox* SpecialDevicesDialog::useSpecialPadCb(int slot) const {
    switch (slot) { case 1: return ui->useSpecial1; case 2: return ui->useSpecial2;
                    case 3: return ui->useSpecial3; case 4: return ui->useSpecial4;
                    default: return nullptr; }
}
QComboBox* SpecialDevicesDialog::specialPadClassCb(int slot) const {
    switch (slot) { case 1: return ui->class1; case 2: return ui->class2;
                    case 3: return ui->class3; case 4: return ui->class4;
                    default: return nullptr; }
}
QCheckBox* SpecialDevicesDialog::legacyCb(int slot) const {
    switch (slot) { case 1: return ui->legacy1; case 2: return ui->legacy2;
                    case 3: return ui->legacy3; case 4: return ui->legacy4;
                    default: return nullptr; }
}
QLabel* SpecialDevicesDialog::statusLbl(int slot) const {
    switch (slot) { case 1: return ui->status1; case 2: return ui->status2;
                    case 3: return ui->status3; case 4: return ui->status4;
                    default: return nullptr; }
}
QPushButton* SpecialDevicesDialog::probeBtn(int slot) const {
    switch (slot) { case 1: return ui->probe1; case 2: return ui->probe2;
                    case 3: return ui->probe3; case 4: return ui->probe4;
                    default: return nullptr; }
}
QLabel* SpecialDevicesDialog::udevWarn(int slot) const {
    switch (slot) { case 1: return ui->warn1; case 2: return ui->warn2;
                    case 3: return ui->warn3; case 4: return ui->warn4;
                    default: return nullptr; }
}

// ----- load / write config ----------------------------------------------------
void SpecialDevicesDialog::loadFromConfig() {
    for (int slot = 1; slot <= 4; ++slot) {
        useSpecialPadCb(slot)->setChecked(Config::getUseSpecialPad(slot));
        const int cls = Config::getSpecialPadClass(slot);
        const int idx = specialPadClassCb(slot)->findData(cls);
        specialPadClassCb(slot)->setCurrentIndex(idx >= 0 ? idx : 0);
        legacyCb(slot)->setChecked(Config::getSpecialPadLegacyPassUSBRawHID(slot));
        updateLegacyRowVisuals(slot);
    }
    ui->kitsLoadedLabel->setText(
        tr("Loaded kit definitions: %1 (drop more into ~/.local/share/shadPS4/kits/*.toml)")
            .arg(Input::HidInstrument::GetLoadedKitCount()));
}

void SpecialDevicesDialog::writeConfig() {
    for (int slot = 1; slot <= 4; ++slot) {
        Config::setUseSpecialPad(slot, useSpecialPadCb(slot)->isChecked());
        Config::setSpecialPadClass(slot,
            specialPadClassCb(slot)->currentData().toInt());
        Config::setSpecialPadLegacyPassUSBRawHID(slot,
            legacyCb(slot)->isChecked());
    }
    // Persist immediately so the next launch picks the values up.
    Config::save(Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "config.toml");
}

void SpecialDevicesDialog::onApply() {
    writeConfig();
}

void SpecialDevicesDialog::onAccept() {
    writeConfig();
    accept();
}

// ----- udev rules detection + install ----------------------------------------
bool SpecialDevicesDialog::udevRulesInstalled() const {
#ifdef __linux__
    std::error_code ec;
    return std::filesystem::exists(kUdevRulesPath, ec);
#else
    // No equivalent permission step needed on other platforms.
    return true;
#endif
}

void SpecialDevicesDialog::refreshUdevState() {
    const bool installed = udevRulesInstalled();
    if (!installed) {
        ui->udevGlobalWarn->setText(
            tr("⚠ udev rules not installed — USB instruments will need sudo to be readable. "
               "Click \"Install Linux udev rules\" below."));
        ui->udevGlobalWarn->setVisible(true);
    } else {
        ui->udevGlobalWarn->setVisible(false);
    }
    ui->installUdevBtn->setEnabled(!installed);
    ui->installUdevBtn->setText(installed
        ? tr("✓ udev rules already installed")
        : tr("Install Linux udev rules (one-time, requires password)"));
    for (int slot = 1; slot <= 4; ++slot) {
        updateLegacyRowVisuals(slot);
    }
}

void SpecialDevicesDialog::onInstallUdev() {
#ifdef __linux__
    // Inline rules text — vendor-wide hidraw/usb access for the known
    // game-instrument VIDs (RedOctane/Sony Licensee, Mad Catz, PDP, Konami).
    static const char* kRulesText =
        "# Generated by shadPS4 Special Devices dialog.\n"
        "SUBSYSTEM==\"usb\",    ATTRS{idVendor}==\"12ba\", TAG+=\"uaccess\", MODE=\"0666\"\n"
        "SUBSYSTEM==\"hidraw\", ATTRS{idVendor}==\"12ba\", TAG+=\"uaccess\", MODE=\"0666\"\n"
        "SUBSYSTEM==\"usb\",    ATTRS{idVendor}==\"1bad\", TAG+=\"uaccess\", MODE=\"0666\"\n"
        "SUBSYSTEM==\"hidraw\", ATTRS{idVendor}==\"1bad\", TAG+=\"uaccess\", MODE=\"0666\"\n"
        "SUBSYSTEM==\"usb\",    ATTRS{idVendor}==\"0e6f\", TAG+=\"uaccess\", MODE=\"0666\"\n"
        "SUBSYSTEM==\"hidraw\", ATTRS{idVendor}==\"0e6f\", TAG+=\"uaccess\", MODE=\"0666\"\n"
        "SUBSYSTEM==\"usb\",    ATTRS{idVendor}==\"1430\", TAG+=\"uaccess\", MODE=\"0666\"\n"
        "SUBSYSTEM==\"hidraw\", ATTRS{idVendor}==\"1430\", TAG+=\"uaccess\", MODE=\"0666\"\n"
        "SUBSYSTEM==\"usb\",    ATTRS{idVendor}==\"1ccf\", TAG+=\"uaccess\", MODE=\"0666\"\n"
        "SUBSYSTEM==\"hidraw\", ATTRS{idVendor}==\"1ccf\", TAG+=\"uaccess\", MODE=\"0666\"\n";

    QProcess proc;
    proc.setProgram("pkexec");
    proc.setArguments({"sh", "-c",
        QStringLiteral("cat > %1 <<'EOF'\n%2EOF\n"
                       "chmod 0644 %1 && "
                       "udevadm control --reload-rules && udevadm trigger")
            .arg(kUdevRulesPath).arg(QString::fromUtf8(kRulesText))});
    if (!proc.startDetached()) {
        QMessageBox::warning(this, tr("Install udev rules"),
                             tr("Could not launch pkexec. Is polkit installed?"));
        return;
    }
    QMessageBox::information(this, tr("Install udev rules"),
        tr("A password prompt should appear. After it completes, unplug and "
           "replug your instrument."));
    QTimer::singleShot(2000, this, [this]() { refreshUdevState(); });
#else
    QMessageBox::information(this, tr("Install udev rules"),
                             tr("Linux-only feature."));
#endif
}

// ----- per-row dynamic visuals ------------------------------------------------
void SpecialDevicesDialog::updateLegacyRowVisuals(int slot) {
    const bool legacyOn = legacyCb(slot)->isChecked();
    const bool udevOk = udevRulesInstalled();
    if (!legacyOn) {
        probeBtn(slot)->setVisible(false);
        udevWarn(slot)->setVisible(false);
        udevWarn(slot)->clear();
        return;
    }
    if (udevOk) {
        probeBtn(slot)->setVisible(true);
        udevWarn(slot)->setVisible(false);
        udevWarn(slot)->clear();
    } else {
        probeBtn(slot)->setVisible(false);
        udevWarn(slot)->setVisible(true);
        udevWarn(slot)->setText(tr("⚠ install udev rules first"));
    }
}

void SpecialDevicesDialog::onLegacyToggled(int slot, bool /*checked*/) {
    updateLegacyRowVisuals(slot);
}

void SpecialDevicesDialog::onRefresh() {
    writeConfig();
    Input::HidInstrument::RescanKits();
    Input::HidInstrument::EnsureInit();
    refreshKitStatus();
    ui->kitsLoadedLabel->setText(
        tr("Loaded kit definitions: %1")
            .arg(Input::HidInstrument::GetLoadedKitCount()));
}

void SpecialDevicesDialog::onProbeKit(int slot) {
    Q_UNUSED(slot);
    KitProbeDialog dlg(this);
    dlg.exec();
    // Hit "Refresh detected kits" to actually open a newly probed TOML
    // without a relaunch; this line just refreshes the count display.
    ui->kitsLoadedLabel->setText(
        tr("Loaded kit definitions: %1").arg(Input::HidInstrument::GetLoadedKitCount()));
}

// ----- runtime kit status -----------------------------------------------------
void SpecialDevicesDialog::refreshKitStatus() {
    // Kick the HID poller so kits get opened now (not when a game launches).
    uint8_t dummy[64];
    std::size_t dummyLen = 0;
    for (int slot = 1; slot <= 4; ++slot) {
        if (Config::getSpecialPadLegacyPassUSBRawHID(slot)) {
            Input::HidInstrument::GetLatestReport(slot, dummy, &dummyLen);
        }
        statusLbl(slot)->setText(tr("(checking...)"));
    }
    QTimer::singleShot(400, this, [this]() {
        for (int slot = 1; slot <= 4; ++slot) {
            QString text;
            if (!Config::getSpecialPadLegacyPassUSBRawHID(slot)) {
                text = QStringLiteral("—");
            } else {
                const std::string k = Input::HidInstrument::GetActiveKitName(slot);
                text = k.empty() ? tr("(no matching kit plugged in)")
                                 : QString::fromStdString(k);
            }
            statusLbl(slot)->setText(text);
        }
    });
}
