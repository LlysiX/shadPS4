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
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <vector>
#include <QDialog>

class QCheckBox;
class QLabel;
class QProgressBar;
class QSlider;
class QSpinBox;
class QTableWidget;
class QWidget;

namespace Ui {
class KitProbeDialog;
}

class KitProbeDialog : public QDialog {
    Q_OBJECT
public:
    explicit KitProbeDialog(QWidget* parent = nullptr);
    ~KitProbeDialog() override;

    struct StepDef {
        QString key;    // json key (e.g. "red_pad")
        QString prompt; // user-facing prompt
        QString kind;   // "velocity", "digital", or "motion"
        bool optional;  // user can skip
        // Per-step sample window length, in ms. Digital button steps
        // (start/select/dpad-left/right) only need a couple of taps so
        // they default to 3 s; fret/velocity/motion need the full 5 s
        // to see range. 0 means "use kStepDurationMs".
        int duration_ms = 0;
    };

    enum class DeviceType {
        Drum,       // 5-lane GH / RB 4-lane no-cymbal — per-color velocity
        ProDrum,    // RB Pro drums — pad and cymbal slots distinct
        Guitar,     // standard 5-fret guitar (PS3 GH/RB, PS4 RB w/o solo)
        GuitarSolo, // 5-fret guitar with upper-neck solo frets (PS4/PS5 RB)
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
        std::array<ByteObs, 64> bytes{};       // per-byte stats
        std::vector<std::vector<uint8_t>> raw; // every observed report
        bool captured = false;
    };

    enum class State {
        SelectDevice,
        Idle, // baseline / "don't touch anything"
        Step, // sampling current step
        Tune, // /v2 per-pad gate + linearity tuning
        Review,
    };

    class PadVuBar; // forward, defined in kit_probe_dialog.cpp

    // One row in the Tune-pads step. Mirrors the toml keys
    // [gate]/[velocity_scaling] expect plus a live VU readout. gate=0
    // means the gate is disabled (no separate enable flag).
    struct PadTuneRow {
        QString toml_key;      // "red", "blue", ...
        int dud_idx = -1;      // -1 = no dud slot (kick under MIDI)
        int raw_byte_idx = -1; // snapshot byte for VU readout / runtime gate
        PadVuBar* vu = nullptr;
        QSlider* gate_slider = nullptr;
        QLabel* gate_value = nullptr;
        QSpinBox* lo_spin = nullptr;
        QSpinBox* hi_spin = nullptr;
        QLabel* raw_label = nullptr;
    };

    void setState(State s);
    void enumerateHidrawDevices();
    bool openDevice(const QString& path, uint16_t vid, uint16_t pid, const QString& name,
                    bool is_xinput);
    void closeDevice();
    void resetByteGrid(int reportLen);
    void updateByteGridCell(int idx, uint8_t value, bool changed);
    void appendReportToCurrentStep(const uint8_t* data, std::size_t len);
    void detectCrossTalkAndWarn();
    void startStep(int idx);
    void finishStep();
    QString deriveKitToml() const;

    // /v2 Tune-pads step: build a per-pad VU+threshold panel from the
    // captured data, hand control back when the user clicks Save.
    void enterTunePhase();
    void buildTuneRowsForKit();
    void seedTuneDefaultsFromCapture();
    void updateTuneVuFromCurrentInputs();
    QString rewriteTomlWithTuneOverrides(const QString& baseToml) const;

    std::unique_ptr<Ui::KitProbeDialog> ui;

    // Device. Exactly one of m_hidDev (SDL_hid_device*), m_xinputDev
    // (SDL_Gamepad*), or m_midiDev (Input::MidiInput opaque handle) is
    // set while a kit is open. All kept as void* so the header doesn't
    // have to pull in SDL3 / midi_input.h.
    void* m_hidDev = nullptr;
    void* m_xinputDev = nullptr;
    void* m_midiDev = nullptr;
    QString m_devicePath;
    QString m_deviceName;
    uint16_t m_vid = 0;
    uint16_t m_pid = 0;
    bool m_isXInput = false;
    bool m_isMidi = false;
    // For MIDI: the port id string ("client:port" on ALSA) the runtime
    // needs to reopen the same device. Travels through the meta record
    // verbatim. Empty for HID/XInput captures.
    QString m_midiPortId;

    // Sampling state
    State m_state = State::SelectDevice;
    int m_currentStep = -1;
    bool m_sampling = false;
    std::chrono::steady_clock::time_point m_stepStart;
    static constexpr int kStepDurationMs = 5000;
    static constexpr int kBaselineDurationMs = 1500;

    // Per-step results. m_results is populated for HID / XInput probes
    // (byte-grid stats + frame buffer). m_midi_results is populated for
    // MIDI probes (raw Note On / Off events per step). Exactly one is
    // populated for any given capture session — the other stays empty.
    // The save path picks which to write based on m_isMidi.
    std::vector<StepResult> m_results;
    std::vector<std::vector<uint8_t>> m_idleRaw;
    std::array<uint8_t, 64> m_lastReport{};
    int m_lastReportLen = 0;
    std::array<int, 64> m_baselineMax{}; // per-byte max observed at idle
    std::array<int, 64> m_baselineMin{}; // per-byte min observed at idle
    std::set<int> m_motionBytes;         // bytes flagged as motion sensors
    // Parallel to m_results: MIDI captures store raw events (note,
    // velocity, on/off, ms since step start) instead of per-byte
    // statistics. Encoded as 4-tuples to dodge a heavier include here.
    struct MidiStepEvent {
        bool on;
        std::uint8_t note;
        std::uint8_t velocity;
        std::uint32_t t_ms;
    };
    struct MidiStepBuf {
        bool captured = false;
        std::vector<MidiStepEvent> events;
        std::chrono::steady_clock::time_point step_started{};
    };
    std::vector<MidiStepBuf> m_midi_results;
    DeviceType m_deviceType = DeviceType::Drum;
    int m_reportLen = 27;

    // Timing
    QTimer* m_tick = nullptr;

    // Tune-pads state. The container is parented to the main dialog
    // and inserted into the existing layout when entered; everything
    // is destroyed with the dialog.
    QWidget* m_tunePanel = nullptr;
    std::vector<PadTuneRow> m_tuneRows;
    std::array<int, 64> m_tuneVuRawPeak{}; // per-byte live peak for VU
};
