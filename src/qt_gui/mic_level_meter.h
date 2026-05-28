// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Discord-style mic level meter: a horizontal bar showing the current
// input level with a movable threshold marker drawn on top. The filled
// portion turns green while the gate is open (level past threshold) and
// dims to grey when the gate is closed, so the user gets immediate
// visual feedback while dialing in the noise-gate sensitivity.
//
// Display-only widget — no Q_OBJECT / signals needed, so it doesn't go
// through moc. settings_dialog.cpp creates one and feeds it values from
// the live capture-preview timer.

#pragma once

#include <algorithm>
#include <QColor>
#include <QPainter>
#include <QWidget>

class MicLevelMeter : public QWidget {
public:
    explicit MicLevelMeter(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(20);
    }

    // All values are on a 0..100 scale matching the dB→bar mapping the
    // settings dialog uses ([-90 dB, 0 dB] -> [0, 100]).
    void setLevel(int level) {
        m_level = std::clamp(level, 0, 100);
        update();
    }
    void setThreshold(int threshold) {
        m_threshold = std::clamp(threshold, 0, 100);
        update();
    }
    void setGateOpen(bool open) {
        m_gate_open = open;
        update();
    }
    void setGateEnabled(bool enabled) {
        m_gate_enabled = enabled;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        const int w = width();
        const int h = height();

        // Track background.
        p.fillRect(rect(), QColor(0x1e, 0x1e, 0x1e));

        // Filled level. Green when the gate would pass this level (or the
        // gate is disabled = always passing), grey otherwise.
        const int fill_w = w * m_level / 100;
        const bool passing = !m_gate_enabled || m_gate_open;
        const QColor fill = passing ? QColor(0x4c, 0xc2, 0x5a)  // green
                                    : QColor(0x6a, 0x6a, 0x6a); // grey
        if (fill_w > 0) {
            p.fillRect(0, 0, fill_w, h, fill);
        }

        // Subtle tick at 25/50/75% so the scale is readable.
        p.setPen(QColor(0x3a, 0x3a, 0x3a));
        for (int pct = 25; pct < 100; pct += 25) {
            const int x = w * pct / 100;
            p.drawLine(x, 0, x, h);
        }

        // Threshold marker — the line the level has to cross to open the
        // gate. Hidden when the gate is disabled.
        if (m_gate_enabled) {
            const int x = std::clamp(w * m_threshold / 100, 0, w - 1);
            p.setPen(QPen(QColor(0xff, 0xc0, 0x30), 2)); // amber
            p.drawLine(x, 0, x, h);
        }

        // Border.
        p.setPen(QColor(0x50, 0x50, 0x50));
        p.drawRect(0, 0, w - 1, h - 1);
    }

private:
    int m_level = 0;
    int m_threshold = 50;
    bool m_gate_open = false;
    bool m_gate_enabled = true;
};
