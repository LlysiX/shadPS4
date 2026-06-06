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

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_joystick.h>

#include "common/config.h"
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
    setWindowTitle(tr("Player Assignment"));
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
    QStringList labels;
    QStringList guids;
    for (int i = 0; i < n; ++i) {
        char buf[33];
        SDL_GUIDToString(SDL_GetJoystickGUIDForID(ids[i]), buf, sizeof(buf));
        const char* nm = SDL_GetJoystickNameForID(ids[i]);
        if (!nm || !*nm) nm = SDL_GetGamepadNameForID(ids[i]);
        const QString name = nm ? QString::fromUtf8(nm) : QStringLiteral("?");
        const QString guid = QString::fromLatin1(buf);
        labels << QStringLiteral("%1 — %2…").arg(name, guid.left(12));
        guids << guid;
    }
    SDL_free(ids);
    bool ok = false;
    const QString choice = QInputDialog::getItem(
        this, tr("Add Gamepad"), tr("Choose a gamepad to bind to Player %1:").arg(slot),
        labels, 0, /*editable=*/false, &ok);
    if (!ok) return;
    const int idx = labels.indexOf(choice);
    if (idx < 0) return;
    Config::PlayerDevice dev;
    dev.kind = Config::PlayerDeviceKind::Gamepad;
    dev.guid = guids[idx].toStdString();
    auto* item = makeDeviceItem(dev);
    m_lists[slot - 1]->addItem(item);
}

void PlayerAssignmentDialog::addKitDevice(int slot) {
    bool ok = false;
    const QString s = QInputDialog::getText(
        this, tr("Add Kit"),
        tr("Enter the kit's VID:PID (e.g. 0x1209:0x2882):"),
        QLineEdit::Normal, QString(), &ok);
    if (!ok || s.isEmpty()) return;
    const QRegularExpression re(
        QStringLiteral("^\\s*0?x?([0-9a-fA-F]{1,4})\\s*:\\s*0?x?([0-9a-fA-F]{1,4})\\s*$"));
    const auto m = re.match(s);
    if (!m.hasMatch()) {
        QMessageBox::warning(this, tr("Bad VID:PID"),
                             tr("Expected format: 0x1209:0x2882 (4 hex digits each)."));
        return;
    }
    Config::PlayerDevice dev;
    dev.kind = Config::PlayerDeviceKind::Kit;
    dev.vid = static_cast<u16>(m.captured(1).toUInt(nullptr, 16));
    dev.pid = static_cast<u16>(m.captured(2).toUInt(nullptr, 16));
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
    QStringList labels;
    QStringList ids;
    for (const auto& p : ports) {
        const QString name = QString::fromStdString(p.name);
        const QString port_id = QString::fromStdString(p.id);
        labels << QStringLiteral("%1  [port %2]").arg(name, port_id);
        ids << port_id;
    }
    bool ok = false;
    const QString choice = QInputDialog::getItem(
        this, tr("Add MIDI"),
        tr("Choose a MIDI input port to bind to Player %1:").arg(slot),
        labels, 0, /*editable=*/false, &ok);
    if (!ok) return;
    const int idx = labels.indexOf(choice);
    if (idx < 0) return;
    Config::PlayerDevice dev;
    dev.kind = Config::PlayerDeviceKind::Midi;
    dev.guid = ids[idx].toStdString();
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
    accept();
}
