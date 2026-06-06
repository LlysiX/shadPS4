// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "player_assignment_dialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QVBoxLayout>

#include <set>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hidapi.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_joystick.h>

#include "common/config.h"
#include "device_picker_dialog.h"
#include "input/controller.h"
#include "input/hid_kit_def.h"
#include "input/input_handler.h"
#include "input/midi_input.h"

namespace {

// Reads each slot's encoded device strings into a parallel array we can
// edit in the dialog without touching Config until OK is clicked. The
// outer index is slot - 1.
using SlotDeviceLists = std::array<std::vector<Config::PlayerDevice>, 4>;

QString deviceDisplayLabel(const Config::PlayerDevice& dev) {
    using Kind = Config::PlayerDeviceKind;
    switch (dev.kind) {
    case Kind::Gamepad: {
        // Try to resolve a friendly name from any currently-connected
        // gamepad with this GUID. Falls back to "Gamepad (<short-guid>)"
        // when the device isn't plugged in.
        int n = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&n);
        QString name;
        for (int i = 0; ids && i < n; ++i) {
            char buf[33];
            SDL_GUIDToString(SDL_GetJoystickGUIDForID(ids[i]), buf, sizeof(buf));
            if (std::string(buf) == dev.guid) {
                const char* nm = SDL_GetJoystickNameForID(ids[i]);
                if (!nm || !*nm) nm = SDL_GetGamepadNameForID(ids[i]);
                if (nm && *nm) name = QString::fromUtf8(nm);
                break;
            }
        }
        if (ids) SDL_free(ids);
        const QString short_guid = QString::fromStdString(dev.guid).left(8);
        if (!name.isEmpty()) {
            return QStringLiteral("Gamepad: %1 (%2…)").arg(name, short_guid);
        }
        return QStringLiteral("Gamepad: %1… (not connected)").arg(short_guid);
    }
    case Kind::Kit:
        return QStringLiteral("Kit: 0x%1:0x%2")
            .arg(dev.vid, 4, 16, QChar('0'))
            .arg(dev.pid, 4, 16, QChar('0'));
    case Kind::Keyboard:
        return QStringLiteral("Keyboard");
    case Kind::Midi:
        return QStringLiteral("MIDI: %1").arg(QString::fromStdString(dev.guid));
    }
    return {};
}

QListWidgetItem* makeDeviceItem(const Config::PlayerDevice& dev) {
    auto* item = new QListWidgetItem(deviceDisplayLabel(dev));
    item->setData(Qt::UserRole, QString::fromStdString(Config::encodePlayerDevice(dev)));
    return item;
}

}  // namespace

PlayerAssignmentDialog::PlayerAssignmentDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Player Assignment Overrides"));
    setModal(true);
    resize(640, 540);

    auto* root = new QVBoxLayout(this);

    auto* help = new QLabel(
        tr("Pin specific devices to PS4 player slots. Multiple devices on "
           "the same slot merge their inputs — e.g. a navigation gamepad + "
           "an RB instrument, or a keyboard + a friend's gamepad both "
           "controlling Player 1.\n"
           "Slots with no assignment fall back to the default first-come-"
           "first-served behaviour."),
        this);
    help->setWordWrap(true);
    help->setStyleSheet(QStringLiteral("color: #888;"));
    root->addWidget(help);

    for (int slot = 1; slot <= 4; ++slot) {
        auto* group = new QGroupBox(tr("Player %1").arg(slot), this);
        auto* g = new QVBoxLayout(group);
        auto* list = new QListWidget(group);
        list->setSelectionMode(QAbstractItemView::SingleSelection);
        m_lists[slot - 1] = list;
        g->addWidget(list);

        auto* btnRow = new QHBoxLayout();
        auto* addGp = new QPushButton(tr("Add Gamepad…"), group);
        auto* addKit = new QPushButton(tr("Add Kit…"), group);
        auto* addKbd = new QPushButton(tr("Add Keyboard"), group);
        auto* addMidi = new QPushButton(tr("Add MIDI…"), group);
        auto* remove = new QPushButton(tr("Remove"), group);
        btnRow->addWidget(addGp);
        btnRow->addWidget(addKit);
        btnRow->addWidget(addKbd);
        btnRow->addWidget(addMidi);
        btnRow->addStretch();
        btnRow->addWidget(remove);
        g->addLayout(btnRow);

        connect(addGp, &QPushButton::clicked, this,
                [this, slot]() { addGamepadDevice(slot); });
        connect(addKit, &QPushButton::clicked, this,
                [this, slot]() { addKitDevice(slot); });
        connect(addKbd, &QPushButton::clicked, this,
                [this, slot]() { addKeyboardDevice(slot); });
        connect(addMidi, &QPushButton::clicked, this,
                [this, slot]() { addMidiDevice(slot); });
        connect(remove, &QPushButton::clicked, this,
                [this, slot]() { removeSelectedDevice(slot); });

        root->addWidget(group);
        refreshSlotList(slot);
    }

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                    Qt::Horizontal, this);
    root->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, this,
            &PlayerAssignmentDialog::onAccept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void PlayerAssignmentDialog::refreshSlotList(int slot) {
    auto* list = m_lists[slot - 1];
    list->clear();
    for (const auto& dev : Config::getPlayerSlotDevices(slot)) {
        list->addItem(makeDeviceItem(dev));
    }
}

void PlayerAssignmentDialog::addGamepadDevice(int slot) {
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    if (!ids || n == 0) {
        if (ids) SDL_free(ids);
        QMessageBox::information(this, tr("No gamepads detected"),
                                 tr("Plug in a controller and try again."));
        return;
    }
    DevicePickerDialog dlg(tr("Add Gamepad"),
        tr("Choose a gamepad to bind to Player %1. Two physically "
           "identical controllers are told apart by their USB port "
           "(device path) — bindings stay tied to the port until you "
           "move the cable.").arg(slot), this);
    for (int i = 0; i < n; ++i) {
        char buf[33];
        SDL_GUIDToString(SDL_GetJoystickGUIDForID(ids[i]), buf, sizeof(buf));
        const char* nm = SDL_GetJoystickNameForID(ids[i]);
        if (!nm || !*nm) nm = SDL_GetGamepadNameForID(ids[i]);
        const QString name = nm ? QString::fromUtf8(nm) : QStringLiteral("?");
        const QString guid = QString::fromLatin1(buf);
        const char* path = SDL_GetJoystickPathForID(ids[i]);
        Config::PlayerDevice dev;
        dev.kind = Config::PlayerDeviceKind::Gamepad;
        dev.guid = guid.toStdString();
        if (path) dev.path = path;
        const QString enc = QString::fromStdString(Config::encodePlayerDevice(dev));
        // Show the path suffix so the user can distinguish identical
        // controllers at a glance.
        QString label = QStringLiteral("%1 — %2…").arg(name, guid.left(12));
        if (path && *path) {
            QString p = QString::fromUtf8(path);
            if (p.size() > 28) p = QStringLiteral("…%1").arg(p.right(28));
            label += QStringLiteral("  [%1]").arg(p);
        }
        dlg.addRow(label, enc, true);
    }
    SDL_free(ids);
    if (dlg.exec() != QDialog::Accepted) return;
    Config::PlayerDevice dev;
    if (!Config::decodePlayerDevice(dlg.chosenEncoded().toStdString(), dev)) return;
    m_lists[slot - 1]->addItem(makeDeviceItem(dev));
}

void PlayerAssignmentDialog::addKitDevice(int slot) {
    // Snapshot which kits the runtime currently has loaded — those are
    // the ones that have been probed via the wizard. Anything detected
    // but not in this set still gets listed so the user sees it exists,
    // but with a hint that it needs probing first.
    std::set<std::pair<u16, u16>> loaded;
    {
        std::lock_guard<std::mutex> lk(Input::HidInstrument::g_kits_mu);
        for (const auto& kd : Input::HidInstrument::g_kits) {
            loaded.insert({kd.vid, kd.pid});
        }
    }
    // Walk every HID device on the host. Collapse duplicates by
    // VID:PID — the same kit often exposes several HID interfaces.
    SDL_hid_init();
    std::set<std::pair<u16, u16>> seen;
    struct Row {
        std::string label;
        u16 vid, pid;
        bool probed;
    };
    std::vector<Row> rows;
    if (auto* head = SDL_hid_enumerate(0, 0)) {
        for (auto* d = head; d; d = d->next) {
            const std::pair<u16, u16> key{d->vendor_id, d->product_id};
            if (seen.count(key)) continue;
            seen.insert(key);
            const std::string mfr =
                d->manufacturer_string
                    ? QString::fromWCharArray(d->manufacturer_string).trimmed().toStdString()
                    : "";
            const std::string prod =
                d->product_string
                    ? QString::fromWCharArray(d->product_string).trimmed().toStdString()
                    : "";
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%04x:%04x  %s %s",
                          d->vendor_id, d->product_id,
                          mfr.empty() ? "?" : mfr.c_str(),
                          prod.c_str());
            Row r;
            r.label = buf;
            r.vid = d->vendor_id;
            r.pid = d->product_id;
            r.probed = loaded.count(key) > 0;
            rows.push_back(std::move(r));
        }
        SDL_hid_free_enumeration(head);
    }
    if (rows.empty()) {
        QMessageBox::information(this, tr("No HID devices detected"),
            tr("Plug in your instrument, then open this dialog again."));
        return;
    }
    DevicePickerDialog dlg(tr("Add Kit"),
        tr("Choose a detected HID device to bind to Player %1. "
           "Greyed-out devices haven't been probed yet — open "
           "Special Devices → probe wizard first.").arg(slot), this);
    for (const auto& r : rows) {
        Config::PlayerDevice dev;
        dev.kind = Config::PlayerDeviceKind::Kit;
        dev.vid = r.vid;
        dev.pid = r.pid;
        const QString enc = QString::fromStdString(Config::encodePlayerDevice(dev));
        const QString suffix =
            r.probed ? tr("  [probed]") : tr("  [needs probing]");
        dlg.addRow(QString::fromStdString(r.label) + suffix, enc, r.probed,
                   r.probed ? QString()
                            : tr("Open Settings → Configure Special Devices "
                                 "→ Probe wizard to register this kit."));
    }
    if (dlg.exec() != QDialog::Accepted) return;
    Config::PlayerDevice dev;
    if (!Config::decodePlayerDevice(dlg.chosenEncoded().toStdString(), dev)) return;
    m_lists[slot - 1]->addItem(makeDeviceItem(dev));
}

void PlayerAssignmentDialog::addKeyboardDevice(int slot) {
    // Only one keyboard makes sense per slot — skip if already present.
    auto* list = m_lists[slot - 1];
    for (int i = 0; i < list->count(); ++i) {
        const QString enc = list->item(i)->data(Qt::UserRole).toString();
        if (enc == QLatin1String("keyboard")) {
            QMessageBox::information(this, tr("Keyboard already added"),
                                     tr("Player %1 already has the keyboard.").arg(slot));
            return;
        }
    }
    Config::PlayerDevice dev;
    dev.kind = Config::PlayerDeviceKind::Keyboard;
    list->addItem(makeDeviceItem(dev));
}

void PlayerAssignmentDialog::addMidiDevice(int slot) {
    const auto ports = Input::MidiInput::EnumerateInputPorts();
    if (ports.empty()) {
        QMessageBox::information(this, tr("No MIDI ports detected"),
            tr("No MIDI input ports were found on this host. Plug in your "
               "module (or a USB-MIDI adapter), open this dialog again."));
        return;
    }
    DevicePickerDialog dlg(tr("Add MIDI"),
        tr("Choose a MIDI input port to bind to Player %1.").arg(slot), this);
    for (const auto& p : ports) {
        const QString name = QString::fromStdString(p.name);
        const QString port_id = QString::fromStdString(p.id);
        Config::PlayerDevice dev;
        dev.kind = Config::PlayerDeviceKind::Midi;
        dev.guid = port_id.toStdString();
        const QString enc = QString::fromStdString(Config::encodePlayerDevice(dev));
        dlg.addRow(QStringLiteral("%1  [port %2]").arg(name, port_id), enc, true);
    }
    if (dlg.exec() != QDialog::Accepted) return;
    Config::PlayerDevice dev;
    if (!Config::decodePlayerDevice(dlg.chosenEncoded().toStdString(), dev)) return;
    m_lists[slot - 1]->addItem(makeDeviceItem(dev));
}

void PlayerAssignmentDialog::removeSelectedDevice(int slot) {
    auto* list = m_lists[slot - 1];
    auto items = list->selectedItems();
    for (auto* it : items) delete list->takeItem(list->row(it));
}

void PlayerAssignmentDialog::onAccept() {
    for (int slot = 1; slot <= 4; ++slot) {
        std::vector<Config::PlayerDevice> devs;
        auto* list = m_lists[slot - 1];
        devs.reserve(list->count());
        for (int i = 0; i < list->count(); ++i) {
            const QString enc = list->item(i)->data(Qt::UserRole).toString();
            Config::PlayerDevice dev;
            if (Config::decodePlayerDevice(enc.toStdString(), dev)) {
                devs.push_back(std::move(dev));
            }
        }
        Config::setPlayerSlotDevices(slot, devs);
    }
    // Apply changes live: relocate connected gamepads whose GUID is now
    // bound to a different slot, then re-parse keybindings so keyboard
    // routing picks up any new Keyboard-slot mapping.
    Input::GameControllers::ApplyAssignmentChanges();
    Input::ParseInputConfig(Config::GetUseUnifiedInputConfig() ? std::string("default")
                                                               : std::string());
    accept();
}
