// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "special_devices_dialog.h"
#include "ui_special_devices_dialog.h"

#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <filesystem>
#include <map>
#include <mutex>
#include <set>

#include <SDL3/SDL_hidapi.h>

#include "common/config.h"
#include "common/path_util.h"
#include "input/hid_instrument.h"
#include "input/hid_kit_def.h"
#include "kit_probe_dialog.h"

namespace {

// PS4 device class enum, see core/libraries/pad/pad.h::OrbisPadDeviceClass.
constexpr struct {
    int value;
    const char* label;
} kSpecialPadClasses[] = {
    {0, "Standard"},   {1, "Guitar"},        {2, "Drum"},  {3, "DJ Turntable"}, {4, "Dancemat"},
    {5, "Navigation"}, {6, "SteeringWheel"}, {7, "Stick"}, {8, "FightStick"},   {9, "Gun"},
};

constexpr const char* kUdevRulesPath = "/etc/udev/rules.d/99-shadps4-instruments.rules";

} // namespace

SpecialDevicesDialog::SpecialDevicesDialog(QWidget* parent)
    : QDialog(parent), ui(new Ui::SpecialDevicesDialog) {
    ui->setupUi(this);

    // The per-player widgets stay in the .ui (loadFromConfig / writeConfig
    // still reference them) but are hidden — that config lives in
    // Player Assignment Overrides now.
    for (int slot = 1; slot <= 4; ++slot) {
        auto* cls = specialPadClassCb(slot);
        for (const auto& item : kSpecialPadClasses) {
            cls->addItem(QString::fromUtf8(item.label), item.value);
        }
        connect(probeBtn(slot), &QPushButton::clicked, this, [this, slot]() { onProbeKit(slot); });
        connect(legacyCb(slot), &QCheckBox::toggled, this,
                [this, slot](bool checked) { onLegacyToggled(slot, checked); });
        useSpecialPadCb(slot)->setVisible(false);
        specialPadClassCb(slot)->setVisible(false);
        legacyCb(slot)->setVisible(false);
        probeBtn(slot)->setVisible(false);
        udevWarn(slot)->setVisible(false);
        statusLbl(slot)->setVisible(false);
    }
    // The grid container is unnamed in the .ui, so walk by child name.
    for (const QString name :
         {QStringLiteral("h0"), QStringLiteral("h1"), QStringLiteral("h2"), QStringLiteral("h3"),
          QStringLiteral("h4"), QStringLiteral("h5"), QStringLiteral("p1Lbl"),
          QStringLiteral("p2Lbl"), QStringLiteral("p3Lbl"), QStringLiteral("p4Lbl")}) {
        if (auto* w = findChild<QWidget*>(name))
            w->setVisible(false);
    }
    ui->helpLabel->setText(
        tr("Per-player special-device config (Special Pad, device class, Legacy raw-HID) "
           "now lives in Settings → Configure Controls → Player Assignment Overrides. "
           "This dialog is the system-wide kit library: see every detected HID device, "
           "probe new instruments, and install Linux udev rules."));

    auto* libraryBox = new QGroupBox(tr("Detected HID instruments"), this);
    auto* libraryLayout = new QVBoxLayout(libraryBox);
    m_kit_list = new QListWidget(libraryBox);
    m_kit_list->setSelectionMode(QAbstractItemView::SingleSelection);
    libraryLayout->addWidget(m_kit_list);
    auto* libraryButtonRow = new QHBoxLayout();
    m_probe_btn = new QPushButton(tr("Probe new kit…"), libraryBox);
    m_probe_btn->setToolTip(tr("Walk the probe wizard to register the selected (or any) "
                               "HID device. The wizard writes a kit TOML into "
                               "~/.local/share/shadPS4/kits which is picked up at next "
                               "launch."));
    libraryButtonRow->addStretch();
    libraryButtonRow->addWidget(m_probe_btn);
    libraryLayout->addLayout(libraryButtonRow);
    ui->rootLayout->insertWidget(ui->rootLayout->count() - 1, libraryBox);
    connect(m_probe_btn, &QPushButton::clicked, this, [this]() {
        KitProbeDialog wiz(this);
        wiz.exec();
        rebuildKitLibrary();
    });
    rebuildKitLibrary();

    connect(ui->installUdevBtn, &QPushButton::clicked, this, &SpecialDevicesDialog::onInstallUdev);
#ifndef __linux__
    // Other platforms grant HID access by default (macOS) or use a different
    // mechanism (Windows). The Linux-flavoured udev installer is meaningless
    // there, so hide it.
    ui->installUdevBtn->setVisible(false);
#endif
    connect(ui->refreshBtn, &QPushButton::clicked, this, &SpecialDevicesDialog::onRefresh);
    connect(ui->buttonBox, &QDialogButtonBox::clicked, this, [this](QAbstractButton* b) {
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

void SpecialDevicesDialog::rebuildKitLibrary() {
    if (!m_kit_list)
        return;
    m_kit_list->clear();

    // Make sure the on-disk kits actually live in g_kits before we
    // try to mark probed/unprobed. The runtime loads them lazily on
    // game launch — without this nudge from the launcher dialog,
    // probed kits would appear as "not probed" until a game runs.
    Input::HidInstrument::EnsureKitsLoaded();

    // Snapshot loaded kits — pair the (vid,pid) key with the kit's
    // human-readable name + device_class so the row can show both.
    struct LoadedInfo {
        std::string name;
        std::string device_class;
    };
    std::map<std::pair<u16, u16>, LoadedInfo> loaded;
    {
        std::lock_guard<std::mutex> lk(Input::HidInstrument::g_kits_mu);
        for (const auto& kd : Input::HidInstrument::g_kits) {
            loaded[{kd.vid, kd.pid}] = {kd.name, kd.device_class};
        }
    }

    SDL_hid_init();
    std::set<std::pair<u16, u16>> seen;
    int detected_count = 0;
    int probed_count = 0;
    if (auto* head = SDL_hid_enumerate(0, 0)) {
        for (auto* d = head; d; d = d->next) {
            const std::pair<u16, u16> key{d->vendor_id, d->product_id};
            if (seen.count(key))
                continue;
            seen.insert(key);
            ++detected_count;

            const auto it = loaded.find(key);
            const bool probed = it != loaded.end();
            if (probed)
                ++probed_count;

            const QString hex = QStringLiteral("%1:%2")
                                    .arg(d->vendor_id, 4, 16, QChar('0'))
                                    .arg(d->product_id, 4, 16, QChar('0'));
            const QString hwName = d->product_string
                                       ? QString::fromWCharArray(d->product_string).trimmed()
                                       : QStringLiteral("?");
            QString label;
            if (probed) {
                label = QStringLiteral("✓ %1   %2  →  %3 (%4)")
                            .arg(hex, hwName, QString::fromStdString(it->second.name),
                                 QString::fromStdString(it->second.device_class));
            } else {
                label = QStringLiteral("✗ %1   %2   [not probed]").arg(hex, hwName);
            }
            auto* item = new QListWidgetItem(label, m_kit_list);
            item->setForeground(probed ? QBrush(QColor(0x4c, 0xc2, 0x5a))
                                       : QBrush(QColor(0xaa, 0xaa, 0xaa)));
            if (probed) {
                item->setToolTip(
                    tr("Probed kit. TOML lives in ~/.local/share/shadPS4/kits. Re-probe by "
                       "selecting the row and clicking \"Probe new kit…\"."));
            } else {
                item->setToolTip(
                    tr("Detected on the HID bus but no kit TOML loaded — click "
                       "\"Probe new kit…\" to register it before assigning to a player slot."));
            }
        }
        SDL_hid_free_enumeration(head);
    }
    if (m_kit_list->count() == 0) {
        auto* item = new QListWidgetItem(tr("No HID devices detected."), m_kit_list);
        item->setForeground(QBrush(QColor(0x88, 0x88, 0x88)));
        item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
    }
    ui->kitsLoadedLabel->setText(
        tr("Detected %1 HID devices, %2 probed, %3 kit TOMLs loaded total")
            .arg(detected_count)
            .arg(probed_count)
            .arg(static_cast<int>(Input::HidInstrument::GetLoadedKitCount())));
}

// ----- widget accessors -------------------------------------------------------
QCheckBox* SpecialDevicesDialog::useSpecialPadCb(int slot) const {
    switch (slot) {
    case 1:
        return ui->useSpecial1;
    case 2:
        return ui->useSpecial2;
    case 3:
        return ui->useSpecial3;
    case 4:
        return ui->useSpecial4;
    default:
        return nullptr;
    }
}
QComboBox* SpecialDevicesDialog::specialPadClassCb(int slot) const {
    switch (slot) {
    case 1:
        return ui->class1;
    case 2:
        return ui->class2;
    case 3:
        return ui->class3;
    case 4:
        return ui->class4;
    default:
        return nullptr;
    }
}
QCheckBox* SpecialDevicesDialog::legacyCb(int slot) const {
    switch (slot) {
    case 1:
        return ui->legacy1;
    case 2:
        return ui->legacy2;
    case 3:
        return ui->legacy3;
    case 4:
        return ui->legacy4;
    default:
        return nullptr;
    }
}
QLabel* SpecialDevicesDialog::statusLbl(int slot) const {
    switch (slot) {
    case 1:
        return ui->status1;
    case 2:
        return ui->status2;
    case 3:
        return ui->status3;
    case 4:
        return ui->status4;
    default:
        return nullptr;
    }
}
QPushButton* SpecialDevicesDialog::probeBtn(int slot) const {
    switch (slot) {
    case 1:
        return ui->probe1;
    case 2:
        return ui->probe2;
    case 3:
        return ui->probe3;
    case 4:
        return ui->probe4;
    default:
        return nullptr;
    }
}
QLabel* SpecialDevicesDialog::udevWarn(int slot) const {
    switch (slot) {
    case 1:
        return ui->warn1;
    case 2:
        return ui->warn2;
    case 3:
        return ui->warn3;
    case 4:
        return ui->warn4;
    default:
        return nullptr;
    }
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
        Config::setSpecialPadClass(slot, specialPadClassCb(slot)->currentData().toInt());
        Config::setSpecialPadLegacyPassUSBRawHID(slot, legacyCb(slot)->isChecked());
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
                           .arg(kUdevRulesPath)
                           .arg(QString::fromUtf8(kRulesText))});
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
    QMessageBox::information(this, tr("Install udev rules"), tr("Linux-only feature."));
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
        tr("Loaded kit definitions: %1").arg(Input::HidInstrument::GetLoadedKitCount()));
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
                text = k.empty() ? tr("(no matching kit plugged in)") : QString::fromStdString(k);
            }
            statusLbl(slot)->setText(text);
        }
    });
}
