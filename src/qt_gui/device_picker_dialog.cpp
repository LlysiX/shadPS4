// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "device_picker_dialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>

namespace {
constexpr int kRoleEncoded = Qt::UserRole + 0;
constexpr int kRoleEnabled = Qt::UserRole + 1;
} // namespace

DevicePickerDialog::DevicePickerDialog(const QString& title, const QString& help_text,
                                       QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(title);
    setModal(true);
    resize(560, 420);

    auto* root = new QVBoxLayout(this);
    if (!help_text.isEmpty()) {
        auto* help = new QLabel(help_text, this);
        help->setWordWrap(true);
        help->setStyleSheet(QStringLiteral("color: #888;"));
        root->addWidget(help);
    }

    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    root->addWidget(m_list);

    auto* bb =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, this);
    m_ok = bb->button(QDialogButtonBox::Ok);
    m_ok->setEnabled(false);
    root->addWidget(bb);

    connect(m_list, &QListWidget::itemSelectionChanged, this,
            &DevicePickerDialog::onSelectionChanged);
    connect(m_list, &QListWidget::itemActivated, this, &DevicePickerDialog::onItemActivated);
    connect(bb, &QDialogButtonBox::accepted, this, &DevicePickerDialog::onAccept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // Poll attached gamepads every 50 ms so we can flash rows in
    // response to button-down events without spinning a thread. The
    // timer only kicks anything when at least one row has registered
    // a joystick id via addRow's optional argument.
    m_poll_timer = new QTimer(this);
    m_poll_timer->setInterval(50);
    connect(m_poll_timer, &QTimer::timeout, this, &DevicePickerDialog::onPollInputs);
    m_poll_timer->start();
}

DevicePickerDialog::~DevicePickerDialog() {
    for (auto& [id, row] : m_gamepad_rows) {
        if (row.gp)
            SDL_CloseGamepad(row.gp);
    }
    m_gamepad_rows.clear();
}

void DevicePickerDialog::addRow(const QString& label, const QString& encoded, bool enabled,
                                const QString& disabled_hint, SDL_JoystickID joystick_id) {
    auto* item = new QListWidgetItem(label, m_list);
    item->setData(kRoleEncoded, encoded);
    item->setData(kRoleEnabled, enabled);
    if (!enabled) {
        // Selectable so the user can click to see the hint, but flagged
        // so onAccept can refuse it. Visual: greyed text.
        Qt::ItemFlags flags = item->flags();
        flags &= ~Qt::ItemIsEnabled;
        flags |= Qt::ItemIsSelectable; // keep selectable for hint display
        item->setFlags(flags);
        item->setForeground(QBrush(QColor(0x77, 0x77, 0x77)));
        if (!disabled_hint.isEmpty())
            item->setToolTip(disabled_hint);
    }
    if (joystick_id != 0) {
        SDL_InitSubSystem(SDL_INIT_GAMEPAD);
        SDL_Gamepad* gp = SDL_OpenGamepad(joystick_id);
        if (gp) {
            const int row_idx = m_list->row(item);
            m_gamepad_rows[joystick_id] = GamepadRow{row_idx, gp, false};
        }
    }
}

void DevicePickerDialog::onPollInputs() {
    if (m_gamepad_rows.empty())
        return;
    SDL_UpdateGamepads();
    // Standard buttons SDL3 exposes — South / East / West / North / LB /
    // RB / Back / Start / Guide / L3 / R3 / D-Pad up..right. Tapping
    // any of them lights up the corresponding row.
    static constexpr SDL_GamepadButton kButtons[] = {
        SDL_GAMEPAD_BUTTON_SOUTH,          SDL_GAMEPAD_BUTTON_EAST,
        SDL_GAMEPAD_BUTTON_WEST,           SDL_GAMEPAD_BUTTON_NORTH,
        SDL_GAMEPAD_BUTTON_BACK,           SDL_GAMEPAD_BUTTON_GUIDE,
        SDL_GAMEPAD_BUTTON_START,          SDL_GAMEPAD_BUTTON_LEFT_STICK,
        SDL_GAMEPAD_BUTTON_RIGHT_STICK,    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
        SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_DPAD_UP,
        SDL_GAMEPAD_BUTTON_DPAD_DOWN,      SDL_GAMEPAD_BUTTON_DPAD_LEFT,
        SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
    };
    for (auto& [id, row] : m_gamepad_rows) {
        if (!row.gp)
            continue;
        bool any_pressed = false;
        for (auto b : kButtons) {
            if (SDL_GetGamepadButton(row.gp, b)) {
                any_pressed = true;
                break;
            }
        }
        auto* item = m_list->item(row.row);
        if (!item)
            continue;
        if (any_pressed && !row.was_lit) {
            item->setBackground(QBrush(QColor(0x4c, 0xc2, 0x5a, 0x80)));
            row.was_lit = true;
        } else if (!any_pressed && row.was_lit) {
            item->setBackground(QBrush(Qt::transparent));
            row.was_lit = false;
        }
    }
}

void DevicePickerDialog::onSelectionChanged() {
    const auto items = m_list->selectedItems();
    if (items.isEmpty()) {
        m_ok->setEnabled(false);
        return;
    }
    m_ok->setEnabled(items.front()->data(kRoleEnabled).toBool());
}

void DevicePickerDialog::onItemActivated(QListWidgetItem* item) {
    if (!item || !item->data(kRoleEnabled).toBool())
        return;
    m_chosen = item->data(kRoleEncoded).toString();
    accept();
}

void DevicePickerDialog::onAccept() {
    const auto items = m_list->selectedItems();
    if (items.isEmpty())
        return;
    auto* item = items.front();
    if (!item->data(kRoleEnabled).toBool())
        return;
    m_chosen = item->data(kRoleEncoded).toString();
    accept();
}
