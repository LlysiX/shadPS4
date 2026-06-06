// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "device_picker_dialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QVBoxLayout>

namespace {
constexpr int kRoleEncoded = Qt::UserRole + 0;
constexpr int kRoleEnabled = Qt::UserRole + 1;
}  // namespace

DevicePickerDialog::DevicePickerDialog(const QString& title,
                                       const QString& help_text,
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

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                    Qt::Horizontal, this);
    m_ok = bb->button(QDialogButtonBox::Ok);
    m_ok->setEnabled(false);
    root->addWidget(bb);

    connect(m_list, &QListWidget::itemSelectionChanged, this,
            &DevicePickerDialog::onSelectionChanged);
    connect(m_list, &QListWidget::itemActivated, this,
            &DevicePickerDialog::onItemActivated);
    connect(bb, &QDialogButtonBox::accepted, this, &DevicePickerDialog::onAccept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void DevicePickerDialog::addRow(const QString& label, const QString& encoded,
                                bool enabled, const QString& disabled_hint) {
    auto* item = new QListWidgetItem(label, m_list);
    item->setData(kRoleEncoded, encoded);
    item->setData(kRoleEnabled, enabled);
    if (!enabled) {
        // Selectable so the user can click to see the hint, but flagged
        // so onAccept can refuse it. Visual: greyed text.
        Qt::ItemFlags flags = item->flags();
        flags &= ~Qt::ItemIsEnabled;
        flags |= Qt::ItemIsSelectable;  // keep selectable for hint display
        item->setFlags(flags);
        item->setForeground(QBrush(QColor(0x77, 0x77, 0x77)));
        if (!disabled_hint.isEmpty()) item->setToolTip(disabled_hint);
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
    if (!item || !item->data(kRoleEnabled).toBool()) return;
    m_chosen = item->data(kRoleEncoded).toString();
    accept();
}

void DevicePickerDialog::onAccept() {
    const auto items = m_list->selectedItems();
    if (items.isEmpty()) return;
    auto* item = items.front();
    if (!item->data(kRoleEnabled).toBool()) return;
    m_chosen = item->data(kRoleEncoded).toString();
    accept();
}
