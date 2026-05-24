// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// In-app probe wizard for legacy PS3/PS4 instruments (drum kits, guitars).
// Walks the user through each input on the kit, samples each for ~5 s, shows
// the raw HID report live in a byte grid with cross-talk warnings, and writes
// a runtime TOML kit definition for the loader in src/input/hid_instrument.
// Uses SDL_hid for I/O so it runs on Linux, Windows, and macOS; on Linux a
// pkexec-driven udev rule grant kicks in when an unprivileged open fails.

#pragma once

#include <array>
#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <vector>
#include <QDialog>

class QTableWidget;

namespace Ui {
class KitProbeDialog;
}

class KitProbeDialog : public QDialog {
    Q_OBJECT
public:
    explicit KitProbeDialog(QWidget* parent = nullptr);
    ~KitProbeDialog() override;

    struct StepDef {
        QString key;       // json key (e.g. "red_pad")
        QString prompt;    // user-facing prompt
        QString kind;      // "velocity", "digital", or "motion"
        bool optional;     // user can skip
    };

    enum class DeviceType {
        Drum,       // 5-lane GH / RB 4-lane no-cymbal — per-color velocity
        ProDrum,    // RB Pro drums — pad and cymbal slots distinct
        Guitar,
    };

private slots:
    void onDeviceListRefresh();
    void onDeviceSelected();
    void onStartProbe();
    void onNextStep();
    void onRedoStep();
    void onSaveResults();
    void onHidReadable();
    void onTickTimer();

private:
    struct ByteObs {
        int min = 0xFF;
        int max = 0;
        int min_nonzero = -1;
        int transitions = 0;
        int samples = 0;
    };
    struct StepResult {
        StepDef def;
        std::array<ByteObs, 64> bytes{};         // per-byte stats
        std::vector<std::vector<uint8_t>> raw;   // every observed report
        bool captured = false;
    };

    enum class State {
        SelectDevice,
        Idle,        // baseline / "don't touch anything"
        Step,        // sampling current step
        Review,
    };

    void setState(State s);
    void enumerateHidrawDevices();
    bool openDevice(const QString& path, uint16_t vid, uint16_t pid,
                    const QString& name);
    void closeDevice();
    void resetByteGrid(int reportLen);
    void updateByteGridCell(int idx, uint8_t value, bool changed);
    void appendReportToCurrentStep(const uint8_t* data, std::size_t len);
    void detectCrossTalkAndWarn();
    void startStep(int idx);
    void finishStep();
    QString deriveKitToml() const;
    QString deriveCalibrationJson() const;

    std::unique_ptr<Ui::KitProbeDialog> ui;

    // Device. Exactly one of m_hidDev (SDL_hid_device*) or m_xinputDev
    // (SDL_Gamepad*) is set while a kit is open. Both kept as void* so the
    // header doesn't have to pull in SDL3 headers.
    void* m_hidDev = nullptr;
    void* m_xinputDev = nullptr;
    QString m_devicePath;
    QString m_deviceName;
    uint16_t m_vid = 0;
    uint16_t m_pid = 0;
    bool m_isXInput = false;

    // Sampling state
    State m_state = State::SelectDevice;
    int m_currentStep = -1;
    bool m_sampling = false;
    std::chrono::steady_clock::time_point m_stepStart;
    static constexpr int kStepDurationMs = 5000;
    static constexpr int kBaselineDurationMs = 1500;

    // Per-step results
    std::vector<StepResult> m_results;
    std::array<uint8_t, 64> m_lastReport{};
    int m_lastReportLen = 0;
    std::array<int, 64> m_baselineMax{};   // per-byte max observed at idle
    std::array<int, 64> m_baselineMin{};   // per-byte min observed at idle
    std::set<int> m_motionBytes;           // bytes flagged as motion sensors
    DeviceType m_deviceType = DeviceType::Drum;
    int m_reportLen = 27;

    // Timing
    QTimer* m_tick = nullptr;
};
