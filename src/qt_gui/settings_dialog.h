// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <span>
#include <QDialog>
#include <QGroupBox>
#include <QPushButton>

#include "common/config.h"
#include "common/path_util.h"
#include "gui_settings.h"
#include "qt_gui/compatibility_info.h"

class QTimer;
struct SDL_AudioStream;
class MicLevelMeter;

namespace Ui {
class SettingsDialog;
}

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(std::shared_ptr<gui_settings> gui_settings,
                            std::shared_ptr<CompatibilityInfoClass> m_compat_info,
                            QWidget* parent = nullptr, bool is_game_running = false,
                            bool is_game_specific = false, std::string gsc_serial = "");
    ~SettingsDialog();

    bool eventFilter(QObject* obj, QEvent* event) override;
    void updateNoteTextEdit(const QString& groupName);

    int exec() override;

signals:
    void LanguageChanged(const QString& locale);
    void CompatibilityChanged();
    void BackgroundOpacityChanged(int opacity);

private:
    void LoadValuesFromConfig();
    void UpdateSettings(bool game_specific = false);
    void SyncRealTimeWidgetstoConfig();
    void InitializeEmulatorLanguages();
    void OnLanguageChanged(int index);
    void OnCursorStateChanged(s16 index);
    void closeEvent(QCloseEvent* event) override;
    void setDefaultValues();
    void VolumeSliderChange(int value);
    void onAudioDeviceChange(bool isAdd);
    void pollSDLevents();

    // Live mic-input preview for the noise-gate UI. Opens an SDL capture
    // stream per user slot (primary + harmony 2/3/4), polls each RMS
    // level on a shared timer, and drives the per-row level bars + gate
    // indicators so each singer can dial in their threshold like
    // Discord's input sensitivity meter. The no-arg start/stop variants
    // act on every slot; the indexed variants act on one.
    void StartMicPreview();
    void StartMicPreview(int slot);
    void StopMicPreview();
    void StopMicPreview(int slot);
    void UpdateMicPreview();
    void UpdateMicGateLabels();

    std::unique_ptr<Ui::SettingsDialog> ui;

    std::map<std::string, int> languages;

    QString defaultTextEdit;

    int initialHeight;

    std::string gs_serial;

    bool is_game_running = false;
    bool is_game_specific = false;
    bool is_game_saving = false;

    std::shared_ptr<gui_settings> m_gui_settings;
    QFuture<void> Polling;

    QTimer* m_mic_preview_timer = nullptr;
    // Per-user-slot preview state. Slot 0 = primary mic (the original
    // single-mic preview widgets in MicGroupBox); slots 1..3 = harmony
    // mics in extraMicsGroupBox. Each slot owns its own SDL capture
    // stream so all 4 can run concurrently while the dialog is open.
    struct MicPreviewSlot {
        SDL_AudioStream* stream = nullptr;
        QString device; // device-data string currently open
        bool gate_open = false;
        std::chrono::steady_clock::time_point last_active{};
        MicLevelMeter* meter = nullptr;
    };
    std::array<MicPreviewSlot, 4> m_mic_previews{};
};
