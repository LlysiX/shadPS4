// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "kit_probe_dialog.h"
#include "ui_kit_probe_dialog.h"

#include <QBrush>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>

#include <SDL3/SDL_hidapi.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <sstream>

#include "common/path_util.h"
#include "input/hid_instrument.h"

namespace {

SDL_hid_device* AsHidDev(void* p) { return static_cast<SDL_hid_device*>(p); }

// First step for guitars only — actively shake/tilt the controller so we can
// identify which bytes are motion-sensor noise and exclude them from later
// per-input analysis. Step key starts with "_motion" so the wizard knows to
// treat the result as a byte-filter, not a real input mapping.
const KitProbeDialog::StepDef& MotionStep() {
    static const KitProbeDialog::StepDef s = {
        "_motion_baseline",
        QObject::tr("Tilt, twist and shake the guitar through its full motion "
                    "range — bytes that move now are the accelerometer / gyro "
                    "and will be ignored in the rest of the probe."),
        "motion", false,
    };
    return s;
}

const std::vector<KitProbeDialog::StepDef>& DrumSteps() {
    static const std::vector<KitProbeDialog::StepDef> steps = {
        {"red_pad",         QObject::tr("Red pad — hit it lots of ways (soft, hard, edges)"), "velocity", false},
        {"blue_pad",        QObject::tr("Blue pad — hit soft and hard"),                       "velocity", false},
        {"green_pad",       QObject::tr("Green pad — hit soft and hard"),                      "velocity", true},
        {"yellow_pad",      QObject::tr("Yellow pad (skip if absent)"),                        "velocity", true},
        {"kick_pedal",      QObject::tr("Kick pedal — press soft and hard"),                   "velocity", false},
        {"kick_pedal_2",    QObject::tr("2nd kick pedal (skip if absent)"),                    "velocity", true},
        {"yellow_cymbal",   QObject::tr("Yellow cymbal"),                                       "velocity", true},
        {"orange_cymbal",   QObject::tr("Orange cymbal"),                                       "velocity", true},
        {"blue_cymbal",     QObject::tr("Blue cymbal (skip if absent)"),                       "velocity", true},
        {"green_cymbal",    QObject::tr("Green cymbal (skip if absent)"),                      "velocity", true},
        {"button_start",    QObject::tr("Start button"),                                        "digital",  false},
        {"button_select",   QObject::tr("Select button"),                                       "digital",  false},
        {"button_ps",       QObject::tr("PS / Home button (skip if absent)"),                   "digital",  true},
        {"button_square",   QObject::tr("Square face button"),                                  "digital",  true},
        {"button_cross",    QObject::tr("Cross / X face button"),                               "digital",  true},
        {"button_circle",   QObject::tr("Circle face button"),                                  "digital",  true},
        {"button_triangle", QObject::tr("Triangle face button"),                                "digital",  true},
        {"dpad_up",         QObject::tr("D-pad UP (hold briefly)"),                             "digital",  false},
        {"dpad_down",       QObject::tr("D-pad DOWN (hold briefly)"),                           "digital",  false},
        {"dpad_left",       QObject::tr("D-pad LEFT (hold briefly)"),                           "digital",  false},
        {"dpad_right",      QObject::tr("D-pad RIGHT (hold briefly)"),                          "digital",  false},
    };
    return steps;
}

const std::vector<KitProbeDialog::StepDef>& GuitarSteps() {
    static const std::vector<KitProbeDialog::StepDef> steps = {
        MotionStep(),
        {"tilt_up",        QObject::tr("Lift the guitar's neck UP (Star Power pose) — hold for the whole window. "
                                       "We use this to figure out which direction means 'tilted up' on your kit."),
                                                                                       "tilt_dir", false},
        {"green_fret",     QObject::tr("GREEN fret — hold and release a few times"),  "digital",  false},
        {"red_fret",       QObject::tr("RED fret"),                                    "digital",  false},
        {"yellow_fret",    QObject::tr("YELLOW fret"),                                 "digital",  false},
        {"blue_fret",      QObject::tr("BLUE fret"),                                   "digital",  false},
        {"orange_fret",    QObject::tr("ORANGE fret"),                                 "digital",  false},
        {"strum_up",       QObject::tr("Strum bar UP"),                                "digital",  false},
        {"strum_down",     QObject::tr("Strum bar DOWN"),                              "digital",  false},
        {"whammy_bar",     QObject::tr("Whammy bar — push and release through full range"), "velocity", false},
        {"touch_slider",   QObject::tr("Touch slider — slide finger across the whole strip"), "velocity", true},
        // (No separate tilt step — tilt is already captured by the motion
        //  baseline step at the start of the walkthrough.)
        {"button_start",   QObject::tr("Start button"),                                "digital",  false},
        {"button_select",  QObject::tr("Select button"),                               "digital",  false},
        {"button_ps",      QObject::tr("PS / Home button (skip if absent)"),           "digital",  true},
        {"dpad_up",        QObject::tr("D-pad UP (skip if absent)"),                   "digital",  true},
        {"dpad_down",      QObject::tr("D-pad DOWN (skip if absent)"),                 "digital",  true},
        {"dpad_left",      QObject::tr("D-pad LEFT (skip if absent)"),                 "digital",  true},
        {"dpad_right",     QObject::tr("D-pad RIGHT (skip if absent)"),                "digital",  true},
    };
    return steps;
}

const std::vector<KitProbeDialog::StepDef>& Steps(KitProbeDialog::DeviceType t) {
    // Drum and ProDrum share the same input step list — Pro mode just emits
    // a different TOML layout downstream (separate cymbal slots).
    return (t == KitProbeDialog::DeviceType::Guitar) ? GuitarSteps() : DrumSteps();
}

QString hexByte(uint8_t v) { return QStringLiteral("%1").arg(v, 2, 16, QChar('0')); }

}  // namespace

KitProbeDialog::KitProbeDialog(QWidget* parent)
    : QDialog(parent), ui(new Ui::KitProbeDialog) {
    ui->setupUi(this);

    m_tick = new QTimer(this);
    m_tick->setInterval(33);  // ~30 Hz UI refresh

    connect(ui->refreshBtn, &QPushButton::clicked, this,
            &KitProbeDialog::onDeviceListRefresh);
    connect(ui->deviceList, &QListWidget::itemSelectionChanged, this,
            &KitProbeDialog::onDeviceSelected);
    connect(ui->startBtn, &QPushButton::clicked, this,
            &KitProbeDialog::onStartProbe);
    connect(ui->nextBtn, &QPushButton::clicked, this,
            &KitProbeDialog::onNextStep);
    connect(ui->beginBtn, &QPushButton::clicked, this,
            &KitProbeDialog::onRedoStep);  // begin & redo do the same thing
    connect(ui->skipBtn, &QPushButton::clicked, this, [this]() {
        if (m_currentStep >= 0 && m_currentStep < (int)m_results.size()) {
            m_results[m_currentStep].captured = false;
        }
        onNextStep();
    });
    connect(ui->saveBtn, &QPushButton::clicked, this,
            &KitProbeDialog::onSaveResults);
    connect(ui->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(m_tick, &QTimer::timeout, this, &KitProbeDialog::onTickTimer);

    onDeviceListRefresh();
    setState(State::SelectDevice);
}

KitProbeDialog::~KitProbeDialog() {
    closeDevice();
}

// ============================================================================
// Device enumeration & open
// ============================================================================

void KitProbeDialog::onDeviceListRefresh() {
    ui->deviceList->clear();
    enumerateHidrawDevices();
    ui->startBtn->setEnabled(ui->deviceList->currentRow() >= 0);
}

void KitProbeDialog::enumerateHidrawDevices() {
    if (SDL_hid_init() != 0) {
        auto* item = new QListWidgetItem(
            tr("SDL_hid_init failed — HID enumeration unavailable"),
            ui->deviceList);
        item->setFlags(Qt::ItemIsEnabled);
        return;
    }
    SDL_hid_device_info* head = SDL_hid_enumerate(0, 0);
    // SDL emits one entry per HID interface; collapse duplicate VID:PID rows
    // (the same kit often appears 2-3 times for separate report interfaces).
    QSet<QString> seenKey;
    for (auto* d = head; d; d = d->next) {
        const QString key = QStringLiteral("%1:%2:%3")
            .arg(d->vendor_id, 4, 16, QChar('0'))
            .arg(d->product_id, 4, 16, QChar('0'))
            .arg(QString::fromUtf8(d->path));
        if (seenKey.contains(key)) continue;
        seenKey.insert(key);
        const QString vid = QString::number(d->vendor_id, 16).rightJustified(4, '0');
        const QString pid = QString::number(d->product_id, 16).rightJustified(4, '0');
        const QString manufacturer =
            d->manufacturer_string
                ? QString::fromWCharArray(d->manufacturer_string).trimmed()
                : QString();
        const QString product =
            d->product_string
                ? QString::fromWCharArray(d->product_string).trimmed()
                : QString();
        const QString label = QStringLiteral("%1:%2  %3 %4")
            .arg(vid).arg(pid)
            .arg(manufacturer.isEmpty() ? QStringLiteral("?") : manufacturer)
            .arg(product);
        auto* item = new QListWidgetItem(label, ui->deviceList);
        item->setData(Qt::UserRole + 0, QString::fromUtf8(d->path));
        item->setData(Qt::UserRole + 1, vid);
        item->setData(Qt::UserRole + 2, pid);
        item->setData(Qt::UserRole + 3,
                      QStringLiteral("%1 %2").arg(manufacturer, product).trimmed());
    }
    SDL_hid_free_enumeration(head);
    // XInput gamepads (Xbox 360 / Xbox One controllers, including X360 GH
    // and RB guitars on Windows). These don't speak HID directly so they
    // wouldn't show up in the SDL_hid_enumerate list.
    for (const auto& xi : Input::HidInstrument::EnumerateXInputDevices()) {
        const QString vid = QString::number(xi.vendor_id, 16).rightJustified(4, '0');
        const QString pid = QString::number(xi.product_id, 16).rightJustified(4, '0');
        const QString label = QStringLiteral("[XInput] %1:%2  %3")
            .arg(vid).arg(pid).arg(QString::fromStdString(xi.name));
        auto* item = new QListWidgetItem(label, ui->deviceList);
        item->setData(Qt::UserRole + 0, QStringLiteral("xinput:%1").arg(xi.instance_id));
        item->setData(Qt::UserRole + 1, vid);
        item->setData(Qt::UserRole + 2, pid);
        item->setData(Qt::UserRole + 3, QString::fromStdString(xi.name));
        item->setData(Qt::UserRole + 4, true);  // marks as XInput source
    }
    if (ui->deviceList->count() == 0) {
        auto* item = new QListWidgetItem(
            tr("(no devices found — plug your instrument in and click Refresh)"),
            ui->deviceList);
        item->setFlags(Qt::ItemIsEnabled);
    }
}

void KitProbeDialog::onDeviceSelected() {
    ui->startBtn->setEnabled(ui->deviceList->currentRow() >= 0);
}

bool KitProbeDialog::openDevice(const QString& path, uint16_t vid, uint16_t pid,
                                const QString& name, bool is_xinput) {
    closeDevice();
    m_isXInput = is_xinput;
    // XInput-source devices: path is "xinput:<instance_id>". Skip the SDL_hid
    // dance and open via the gamepad API.
    if (m_isXInput) {
        bool ok = false;
        const int id = path.section(':', 1, 1).toInt(&ok);
        if (!ok) {
            QMessageBox::warning(this, tr("Open failed"),
                                 tr("Could not parse XInput instance id from %1").arg(path));
            return false;
        }
        m_xinputDev = Input::HidInstrument::OpenXInputGamepad(id);
        if (!m_xinputDev) {
            QMessageBox::warning(this, tr("Open failed"),
                tr("SDL_OpenGamepad failed for instance %1.").arg(id));
            return false;
        }
        m_devicePath = path;
        m_deviceName = name;
        m_vid = vid;
        m_pid = pid;
        return true;
    }
    SDL_hid_device* dev = SDL_hid_open_path(path.toUtf8().constData());
    if (!dev) {
        // Fall back to VID:PID open in case the path-based open isn't
        // supported on this backend.
        dev = SDL_hid_open(vid, pid, nullptr);
    }
    if (!dev) {
#ifdef __linux__
        // Most failures on Linux are permission denied — offer to add a udev
        // rule for this VID:PID via pkexec. Other platforms typically grant
        // HID access by default, so there's nothing actionable to offer.
        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(tr("Cannot open device"));
        box.setText(tr("SDL_hid_open failed for %1:%2.\n\n"
                       "On Linux this usually means permission denied — "
                       "add a udev rule so this device is readable without "
                       "sudo? (You'll be asked for your password, then need "
                       "to unplug and replug.)")
                       .arg(vid, 4, 16, QChar('0'))
                       .arg(pid, 4, 16, QChar('0')));
        auto* grant = box.addButton(tr("Grant access (one-time)"),
                                    QMessageBox::AcceptRole);
        box.addButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() == grant) {
            const QString line = QStringLiteral(
                "SUBSYSTEM==\"hidraw\", "
                "ATTRS{idVendor}==\"%1\", ATTRS{idProduct}==\"%2\", "
                "TAG+=\"uaccess\", MODE=\"0666\"")
                .arg(vid, 4, 16, QChar('0'))
                .arg(pid, 4, 16, QChar('0'));
            QProcess proc;
            proc.setProgram("pkexec");
            proc.setArguments({
                "sh", "-c",
                QStringLiteral(
                    "touch /etc/udev/rules.d/99-shadps4-instruments.rules && "
                    "if ! grep -q 'idVendor==\"%1\".*idProduct==\"%2\"' "
                    "/etc/udev/rules.d/99-shadps4-instruments.rules; then "
                    "echo '%3' >> /etc/udev/rules.d/99-shadps4-instruments.rules; "
                    "fi && udevadm control --reload-rules && udevadm trigger")
                    .arg(vid, 4, 16, QChar('0'))
                    .arg(pid, 4, 16, QChar('0'))
                    .arg(line)
            });
            if (proc.startDetached()) {
                QMessageBox::information(this, tr("Rule added"),
                    tr("Rule added. Unplug and replug the device, then "
                       "click Refresh and try again."));
            } else {
                QMessageBox::warning(this, tr("Install failed"),
                    tr("Could not launch pkexec. Is polkit installed?"));
            }
        }
#else
        QMessageBox::warning(this, tr("Open failed"),
            tr("SDL_hid_open failed for %1:%2. The device may be claimed by "
               "another application.")
                .arg(vid, 4, 16, QChar('0')).arg(pid, 4, 16, QChar('0')));
#endif
        return false;
    }
    SDL_hid_set_nonblocking(dev, 1);
    m_hidDev = dev;
    m_devicePath = path;
    m_deviceName = name;
    m_vid = vid;
    m_pid = pid;
    return true;
}

void KitProbeDialog::closeDevice() {
    if (m_hidDev) {
        SDL_hid_close(AsHidDev(m_hidDev));
        m_hidDev = nullptr;
    }
    if (m_xinputDev) {
        Input::HidInstrument::CloseXInputGamepad(m_xinputDev);
        m_xinputDev = nullptr;
    }
    m_devicePath.clear();
    m_deviceName.clear();
    m_vid = m_pid = 0;
    m_isXInput = false;
}

// ============================================================================
// State machine
// ============================================================================

void KitProbeDialog::setState(State s) {
    m_state = s;
    switch (s) {
    case State::SelectDevice:
        ui->pages->setCurrentWidget(ui->pageSelect);
        break;
    case State::Idle:
    case State::Step:
        ui->pages->setCurrentWidget(ui->pageWalk);
        break;
    case State::Review:
        ui->pages->setCurrentWidget(ui->pageReview);
        ui->reviewText->setPlainText(deriveKitToml());
        break;
    }
}

void KitProbeDialog::onStartProbe() {
    auto* item = ui->deviceList->currentItem();
    if (!item) return;
    const QString path = item->data(Qt::UserRole + 0).toString();
    const QString vidStr = item->data(Qt::UserRole + 1).toString();
    const QString pidStr = item->data(Qt::UserRole + 2).toString();
    const QString name = item->data(Qt::UserRole + 3).toString();
    const bool is_xinput = item->data(Qt::UserRole + 4).toBool();
    bool ok1 = false, ok2 = false;
    const uint16_t vid = static_cast<uint16_t>(vidStr.toUInt(&ok1, 16));
    const uint16_t pid = static_cast<uint16_t>(pidStr.toUInt(&ok2, 16));
    if (!ok1 || !ok2) {
        QMessageBox::warning(this, tr("Bad VID/PID"),
                             tr("Could not parse %1:%2.").arg(vidStr, pidStr));
        return;
    }
    if (!openDevice(path, vid, pid, name, is_xinput)) return;

    // Init step results table
    m_results.clear();
    switch (ui->deviceTypeCombo->currentIndex()) {
        case 0:  m_deviceType = DeviceType::Drum;    break;  // 5-lane / no cymbals
        case 1:  m_deviceType = DeviceType::ProDrum; break;  // Pro drums
        case 2:  m_deviceType = DeviceType::Guitar;  break;
        default: m_deviceType = DeviceType::Drum;    break;
    }
    for (const auto& s : Steps(m_deviceType)) m_results.push_back({s, {}, {}, false});
    m_currentStep = -1;
    std::fill(m_baselineMax.begin(), m_baselineMax.end(), 0);
    std::fill(m_baselineMin.begin(), m_baselineMin.end(), 0xFF);
    m_motionBytes.clear();
    resetByteGrid(m_reportLen);

    // Baseline: 1.5s of quiet sampling so we know idle noise band.
    ui->stepHeader->setText(tr("Baseline (1/%1)").arg(Steps(m_deviceType).size() + 1));
    ui->stepPrompt->setText(
        tr("Don't touch the kit. Capturing idle noise for ~1.5 s..."));
    ui->stepProgress->setMaximum(kBaselineDurationMs);
    ui->stepProgress->setValue(0);
    ui->crossTalkLabel->clear();
    ui->beginBtn->setEnabled(false);
    ui->beginBtn->setText(tr("Begin sampling (5 s)"));
    ui->nextBtn->setEnabled(false);
    ui->skipBtn->setEnabled(false);
    m_sampling = false;
    m_stepStart = std::chrono::steady_clock::now();
    setState(State::Idle);
    m_tick->start();
}

void KitProbeDialog::startStep(int idx) {
    if (idx < 0 || idx >= (int)m_results.size()) return;
    m_currentStep = idx;
    auto& r = m_results[idx];
    r.bytes = {};
    r.raw.clear();
    r.captured = false;
    ui->stepHeader->setText(tr("Step %1/%2 — %3")
                                .arg(idx + 2)
                                .arg(Steps(m_deviceType).size() + 1)
                                .arg(r.def.key));
    ui->stepPrompt->setText(
        r.def.prompt +
        tr("\n\nRead the prompt, get ready, then click \"Begin sampling\". "
           "You'll have 5 s to wail on it lots of ways. Click Begin again "
           "to redo if needed."));
    ui->stepProgress->setMaximum(kStepDurationMs);
    ui->stepProgress->setValue(0);
    ui->crossTalkLabel->clear();
    ui->beginBtn->setText(tr("Begin sampling (5 s)"));
    ui->beginBtn->setEnabled(true);
    ui->nextBtn->setEnabled(false);
    ui->skipBtn->setEnabled(r.def.optional);
    m_sampling = false;
    setState(State::Step);
}

// Actually start (or restart) the 5 s capture window for the current step.
// Called both for the first attempt and for redos — same behaviour.
void KitProbeDialog::onRedoStep() {
    if (m_state != State::Step || m_currentStep < 0) return;
    auto& r = m_results[m_currentStep];
    r.bytes = {};
    r.raw.clear();
    r.captured = false;
    ui->stepProgress->setValue(0);
    ui->crossTalkLabel->clear();
    ui->beginBtn->setText(tr("Sampling..."));
    ui->beginBtn->setEnabled(false);
    ui->nextBtn->setEnabled(false);
    m_sampling = true;
    m_stepStart = std::chrono::steady_clock::now();
}

void KitProbeDialog::finishStep() {
    if (m_currentStep < 0 || m_currentStep >= (int)m_results.size()) return;
    auto& r = m_results[m_currentStep];
    r.captured = true;
    m_sampling = false;
    ui->stepProgress->setValue(ui->stepProgress->maximum());
    ui->beginBtn->setText(tr("Redo this step"));
    ui->beginBtn->setEnabled(true);
    ui->nextBtn->setEnabled(true);
    ui->skipBtn->setEnabled(false);
    detectCrossTalkAndWarn();
}

void KitProbeDialog::detectCrossTalkAndWarn() {
    // If this is the synthetic motion-baseline step, record which bytes moved
    // (those are the motion sensor) and DON'T treat the movement as
    // cross-talk — that's the whole point of the step.
    if (m_currentStep >= 0) {
        const auto& r = m_results[m_currentStep];
        if (r.def.kind == "motion") {
            m_motionBytes.clear();
            int counted = 0;
            for (int i = 0; i < m_reportLen; ++i) {
                const auto& b = r.bytes[i];
                if (b.samples == 0) continue;
                if ((b.max - b.min) > 3) {
                    m_motionBytes.insert(i);
                    ++counted;
                }
            }
            ui->crossTalkLabel->setText(
                tr("Flagged %1 byte(s) as motion sensor — they will be ignored "
                   "in the remaining steps.").arg(counted));
            return;
        }
    }
    // For each step, bytes that moved are "expected" if their value range is
    // wide (real input). Bytes that moved a tiny amount are bleed-through.
    // Motion bytes (flagged at baseline) are skipped — they jitter naturally.
    if (m_currentStep < 0) return;
    const auto& r = m_results[m_currentStep];
    QStringList warnings;
    for (int i = 0; i < m_reportLen; ++i) {
        if (m_motionBytes.count(i)) continue;
        const auto& b = r.bytes[i];
        if (b.samples == 0) continue;
        const int range = b.max - b.min;
        if (range >= 0x40) continue;  // real input — large swing
        if (b.max <= m_baselineMax[i] + 1) continue;  // within noise
        if (b.max < 0x10) continue;  // too tiny to call bleed
        warnings << tr("byte %1 jittered up to 0x%2 (baseline max 0x%3)")
                        .arg(i).arg(b.max, 2, 16, QChar('0'))
                        .arg(m_baselineMax[i], 2, 16, QChar('0'));
    }
    if (!warnings.isEmpty()) {
        ui->crossTalkLabel->setText(
            tr("⚠ Possible cross-talk / bleed: %1. Redo if it looked off.")
                .arg(warnings.join("; ")));
    }
}

void KitProbeDialog::onNextStep() {
    if (m_state == State::Idle) {
        // Baseline done — snapshot baseline band and begin step 0.
        // (Already populated by appendReportToCurrentStep using a fake step
        //  — actually we used m_baselineMax tracking directly in onHidReadable.)
        startStep(0);
        return;
    }
    if (m_state == State::Step) {
        const int next = m_currentStep + 1;
        if (next >= (int)m_results.size()) {
            m_tick->stop();
            setState(State::Review);
            return;
        }
        startStep(next);
    }
}


// ============================================================================
// HID reading & live byte grid
// ============================================================================

void KitProbeDialog::onHidReadable() {
    // XInput path: poll the gamepad once per tick and treat the synthetic
    // 9-byte report exactly like an HID read.
    if (m_xinputDev) {
        uint8_t buf[Input::HidInstrument::kXInputReportLen];
        Input::HidInstrument::PollXInputGamepad(m_xinputDev, buf);
        const int n = (int)Input::HidInstrument::kXInputReportLen;
        if (m_reportLen < n) {
            m_reportLen = n;
            resetByteGrid(m_reportLen);
        }
        for (int i = 0; i < n; ++i) {
            const bool changed = (i >= m_lastReportLen) || (m_lastReport[i] != buf[i]);
            updateByteGridCell(i, buf[i], changed);
        }
        std::memcpy(m_lastReport.data(), buf, n);
        m_lastReportLen = n;
        if (m_state == State::Idle) {
            for (int i = 0; i < n; ++i) {
                if (buf[i] > m_baselineMax[i]) m_baselineMax[i] = buf[i];
                if (buf[i] < m_baselineMin[i]) m_baselineMin[i] = buf[i];
            }
        } else if (m_state == State::Step && m_sampling) {
            appendReportToCurrentStep(buf, (std::size_t)n);
        }
        return;
    }
    if (!m_hidDev) return;
    uint8_t buf[64];
    while (true) {
        int n = SDL_hid_read_timeout(AsHidDev(m_hidDev), buf, sizeof(buf), 0);
        if (n <= 0) break;
        if (n > (int)m_lastReport.size()) n = (int)m_lastReport.size();

        // Update live grid (highlight changed bytes).
        if (m_reportLen < (int)n) {
            m_reportLen = (int)n;
            resetByteGrid(m_reportLen);
        }
        for (int i = 0; i < n; ++i) {
            const bool changed = (i >= m_lastReportLen) || (m_lastReport[i] != buf[i]);
            updateByteGridCell(i, buf[i], changed);
        }
        std::memcpy(m_lastReport.data(), buf, n);
        m_lastReportLen = (int)n;

        // Accumulate into current step or baseline.
        if (m_state == State::Idle) {
            for (int i = 0; i < n; ++i) {
                if (buf[i] > m_baselineMax[i]) m_baselineMax[i] = buf[i];
                if (buf[i] < m_baselineMin[i]) m_baselineMin[i] = buf[i];
            }
        } else if (m_state == State::Step && m_sampling) {
            appendReportToCurrentStep(buf, (std::size_t)n);
        }
    }
}

void KitProbeDialog::appendReportToCurrentStep(const uint8_t* data, std::size_t len) {
    if (m_currentStep < 0 || m_currentStep >= (int)m_results.size()) return;
    auto& r = m_results[m_currentStep];
    r.raw.emplace_back(data, data + len);
    for (std::size_t i = 0; i < len; ++i) {
        auto& b = r.bytes[i];
        if (data[i] < b.min) b.min = data[i];
        if (data[i] > b.max) b.max = data[i];
        if (data[i] != 0 && (b.min_nonzero < 0 || data[i] < b.min_nonzero))
            b.min_nonzero = data[i];
        ++b.transitions;
        ++b.samples;
    }
}

void KitProbeDialog::resetByteGrid(int reportLen) {
    auto* g = ui->byteGrid;
    g->setColumnCount(reportLen);
    g->setRowCount(2);
    for (int i = 0; i < reportLen; ++i) {
        auto* hdr = new QTableWidgetItem(QString::number(i));
        hdr->setFlags(Qt::ItemIsEnabled);
        hdr->setForeground(QBrush(QColor("#888")));
        g->setItem(0, i, hdr);
        auto* cell = new QTableWidgetItem("00");
        cell->setFlags(Qt::ItemIsEnabled);
        cell->setForeground(QBrush(QColor("#ccc")));
        g->setItem(1, i, cell);
        g->setColumnWidth(i, 28);
    }
    g->resizeRowsToContents();
}

void KitProbeDialog::updateByteGridCell(int idx, uint8_t value, bool changed) {
    auto* g = ui->byteGrid;
    if (idx >= g->columnCount()) return;
    auto* cell = g->item(1, idx);
    if (!cell) return;
    cell->setText(hexByte(value));
    if (changed) {
        cell->setBackground(QBrush(QColor(value ? "#5a7a3a" : "#3a3a5a")));
    } else if (value == 0) {
        cell->setBackground(QBrush(QColor("#222")));
    } else {
        cell->setBackground(QBrush(QColor("#444")));
    }
}

void KitProbeDialog::onTickTimer() {
    // Drain whatever the kit has sent since the last tick. ~30 Hz polling is
    // plenty for the live byte grid; the per-step samples are still captured
    // at whatever rate the device emits because SDL_hid buffers internally.
    onHidReadable();

    using clock = std::chrono::steady_clock;
    const int elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            clock::now() - m_stepStart).count();
    if (m_state == State::Idle) {
        ui->stepProgress->setValue(std::min(elapsed, kBaselineDurationMs));
        if (elapsed >= kBaselineDurationMs) {
            ui->stepPrompt->setText(
                tr("Baseline captured. Click Next to begin the walk-through."));
            ui->nextBtn->setEnabled(true);
        }
    } else if (m_state == State::Step && m_sampling) {
        ui->stepProgress->setValue(std::min(elapsed, kStepDurationMs));
        if (elapsed >= kStepDurationMs) {
            finishStep();
        }
    }
}

// ============================================================================
// Output: JSON (calibration) and TOML (runtime kit def)
// ============================================================================

QString KitProbeDialog::deriveCalibrationJson() const {
    std::ostringstream os;
    os << "{\n";
    os << "  \"schema\": \"shadps4-legacy-instrument-map/v1\",\n";
    os << "  \"device\": {\n"
       << "    \"vendor_id\": \"0x" << std::hex << m_vid << "\",\n"
       << "    \"product_id\": \"0x" << std::hex << m_pid << "\",\n"
       << "    \"name\": \"" << m_deviceName.toStdString() << "\",\n"
       << "    \"hidraw_path\": \"" << m_devicePath.toStdString() << "\"\n"
       << "  },\n";
    os << std::dec;
    os << "  \"report_length\": " << m_reportLen << ",\n";
    os << "  \"mapping\": {\n";
    bool first = true;
    for (const auto& r : m_results) {
        if (!r.captured) continue;
        if (!first) os << ",\n";
        first = false;
        os << "    \"" << r.def.key.toStdString() << "\": {\n";
        os << "      \"kind\": \"" << r.def.kind.toStdString() << "\",\n";
        // Pick the "velocity byte" = byte with the largest range outside b0/b1/b2.
        int vel_byte = -1, vel_max = 0;
        for (int i = 3; i < m_reportLen; ++i) {
            if (m_motionBytes.count(i)) continue;  // skip motion sensor noise
            const auto& b = r.bytes[i];
            if (b.samples == 0) continue;
            if ((b.max - b.min) > vel_max && b.max >= 0x10 &&
                i != 26 /* tag */ && (i < 3 || i > 4) /* not sticks */) {
                vel_max = b.max - b.min;
                vel_byte = i;
            }
        }
        os << "      \"velocity_byte\": ";
        if (vel_byte >= 0) os << vel_byte; else os << "null";
        os << ",\n";
        // Flag byte/mask: any rising bit in raw[0] or raw[1] during step
        for (int fb : {0, 1, 2}) {
            const auto& b = r.bytes[fb];
            if (b.samples == 0 || b.max == 0) continue;
            os << "      \"flag_byte\": " << fb << ",\n";
            os << "      \"flag_mask\": \"0x" << std::hex << b.max
               << "\",\n" << std::dec;
            break;
        }
        os << "      \"observed_min\": " << (vel_byte >= 0 ? r.bytes[vel_byte].min : 0) << ",\n";
        os << "      \"observed_max\": " << (vel_byte >= 0 ? r.bytes[vel_byte].max : 0) << ",\n";
        os << "      \"sample_count\": " << (vel_byte >= 0 ? r.bytes[vel_byte].samples : 0) << "\n";
        os << "    }";
    }
    os << "\n  }\n";
    os << "}\n";
    return QString::fromStdString(os.str());
}

QString KitProbeDialog::deriveKitToml() const {
    // Pick the byte with the widest range during a step. Skips counter / accel
    // bytes (non-zero or non-centered baseline) and noise (peak below 0x20).
    auto velByte = [&](const QString& key) -> int {
        for (const auto& r : m_results) {
            if (r.def.key != key || !r.captured) continue;
            int best = -1, best_range = 0;
            for (int i = 3; i < m_reportLen; ++i) {
                if (m_motionBytes.count(i)) continue;
                const auto& b = r.bytes[i];
                if (b.samples == 0) continue;
                if (b.max < 0x20) continue;
                const int baseline_floor = m_baselineMin[i];
                const int baseline_ceil  = m_baselineMax[i];
                const bool quiet_at_idle =
                    (baseline_ceil <= 4) ||
                    (baseline_floor >= 0x7C && baseline_ceil <= 0x84);
                if (!quiet_at_idle) continue;
                const int range = b.max - b.min;
                if (range > best_range && i != 26) {
                    best_range = range;
                    best = i;
                }
            }
            return best;
        }
        return -1;
    };
    auto flagBit = [&](const QString& key, int rawByte) -> uint8_t {
        for (const auto& r : m_results) {
            if (r.def.key != key || !r.captured) continue;
            const auto& b = r.bytes[rawByte];
            if (b.samples == 0) return 0;
            return static_cast<uint8_t>(b.max);
        }
        return 0;
    };
    // Scan every byte for the one that gained exactly one set bit during the
    // step (raw[i] went from baseline to baseline | (1 << k)). Used to find
    // fret / face-button bits without hardcoding raw[0] — PS4 RB and PS5
    // Riffmaster put frets at byte 43/46, not 0 (which is the report ID).
    // Returns (byte_index, bit_mask) or (-1, 0).
    auto detectFlagByteAndBit = [&](const QString& key) -> std::pair<int, uint8_t> {
        for (const auto& r : m_results) {
            if (r.def.key != key || !r.captured) continue;
            int best_byte = -1;
            uint8_t best_mask = 0;
            int best_score = 0;
            for (int i = 0; i < m_reportLen; ++i) {
                if (m_motionBytes.count(i)) continue;
                const auto& b = r.bytes[i];
                if (b.samples == 0) continue;
                const int baseline = m_baselineMax[i];
                if (b.max <= baseline) continue;
                // bits that turned on during the step (not present at baseline)
                const int diff = b.max & ~baseline;
                if (diff == 0) continue;
                // Prefer single-bit deltas: real button flags toggle exactly
                // one bit. Multi-bit deltas usually mean an analog axis or HAT.
                if ((diff & (diff - 1)) != 0) continue;
                // Score by how often this byte saw the same delta — buttons
                // assert cleanly, noise jitters.
                const int score = b.transitions;
                if (score > best_score) {
                    best_score = score;
                    best_byte = i;
                    best_mask = static_cast<uint8_t>(diff);
                }
            }
            return {best_byte, best_mask};
        }
        return {-1, 0};
    };
    auto detectHatByte = [&]() -> int {
        static const QStringList keys = {"dpad_up", "dpad_down",
                                         "dpad_left", "dpad_right",
                                         "strum_up", "strum_down"};
        for (const auto& r : m_results) {
            if (!r.captured || !keys.contains(r.def.key)) continue;
            for (int i = 0; i < m_reportLen; ++i) {
                if (m_motionBytes.count(i)) continue;
                const auto& b = r.bytes[i];
                if (b.samples == 0) continue;
                if (b.min <= 7 && b.max <= 0x0F && (b.max - b.min) > 0) {
                    return i;
                }
            }
        }
        return 2;  // sensible default
    };
    const int hatByte = detectHatByte();
    // Detect which byte+bit the Select/Start buttons toggle. PS3 GH/RB
    // guitars use byte 1; PS4 RB / PS5 Riffmaster use byte 9.
    auto [selByte, b_sel] = detectFlagByteAndBit("button_select");
    auto [staByte, b_sta] = detectFlagByteAndBit("button_start");
    if (selByte < 0) { selByte = 1; }
    if (staByte < 0) { staByte = 1; }

    struct DudEntry { int idx; int rawByte; const char* comment; };
    std::vector<DudEntry> dudPlan;
    struct ButtonBit { int byte; uint8_t mask; const char* name; const char* origin; };
    std::vector<ButtonBit> button_bits;
    struct ScaleEntry { int dudIdx; const char* stepKey; const char* comment; };
    std::vector<ScaleEntry> scalePlan;
    const char* deviceClass = "drum";

    if (m_deviceType == DeviceType::Guitar) {
        deviceClass = "guitar";
        dudPlan = {
            {2, velByte("whammy_bar"),    "whammy bar"},
            {3, velByte("touch_slider"),  "touch slider"},
        };
        struct FretMap { const char* step; const char* name; const char* origin; };
        const FretMap fretMaps[] = {
            {"green_fret",  "cross",    "green fret"},
            {"red_fret",    "circle",   "red fret"},
            {"yellow_fret", "triangle", "yellow fret"},
            {"blue_fret",   "square",   "blue fret"},
            {"orange_fret", "l1",       "orange fret"},
        };
        for (const auto& fm : fretMaps) {
            auto [byte, mask] = detectFlagByteAndBit(fm.step);
            if (byte < 0) continue;
            button_bits.push_back({byte, mask, fm.name, fm.origin});
        }
        scalePlan = {};
    } else {
        deviceClass = "drum";
        dudPlan = {
            {2, velByte("yellow_cymbal"), "yellow velocity"},
            {3, velByte("red_pad"),       "red velocity"},
            {4, velByte("green_pad"),     "green velocity"},
            {5, velByte("blue_pad"),      "blue velocity"},
            {6, velByte("kick_pedal"),    "kick velocity"},
            {7, velByte("orange_cymbal"), "orange velocity"},
        };
        const uint8_t b_sq = flagBit("button_square",   0);
        const uint8_t b_cr = flagBit("button_cross",    0);
        const uint8_t b_ci = flagBit("button_circle",   0);
        const uint8_t b_tr = flagBit("button_triangle", 0);
        const uint8_t face = b_sq | b_cr | b_ci | b_tr;
        const uint8_t b_kick   = flagBit("kick_pedal",   0) & ~face;
        const uint8_t b_orange = flagBit("orange_cymbal", 0) & ~face;
        button_bits = {
            {0, b_sq,     "square",   "blue pad"},
            {0, b_cr,     "cross",    "green pad"},
            {0, b_ci,     "circle",   "red pad"},
            {0, b_tr,     "triangle", "yellow pad / yellow cymbal"},
            {0, b_kick,   "l1",       "kick pedal"},
            {0, b_orange, "r1",       "orange cymbal (5th lane in GH-mode)"},
        };
        scalePlan = {
            {2, "yellow_cymbal", "yellow"},
            {3, "red_pad",       "red"},
            {4, "green_pad",     "green"},
            {5, "blue_pad",      "blue"},
            {6, "kick_pedal",    "kick"},
            {7, "orange_cymbal", "orange"},
        };
    }

    int dud[12];
    dud[0]  = 0;
    dud[1]  = 1;
    for (int i = 2; i <= 7; ++i) dud[i] = -1;
    for (const auto& e : dudPlan) dud[e.idx] = e.rawByte;
    dud[8]  = hatByte;
    dud[9]  = -1;
    dud[10] = -1;
    dud[11] = std::max(0, m_reportLen - 1);

    std::ostringstream os;
    os << "# Generated by KitProbeDialog. Picked up at next launch.\n\n";
    os << "schema       = \"shadps4-legacy-instrument/v1\"\n";
    os << "vendor_id    = \"0x" << std::hex << m_vid << "\"\n";
    os << "product_id   = \"0x" << m_pid << "\"\n";
    os << std::dec;
    os << "name         = \"" << m_deviceName.toStdString() << "\"\n";
    os << "device_class = \"" << deviceClass << "\"\n";
    if (m_isXInput) {
        os << "source       = \"xinput\"\n";
    }
    os << "report_length = " << m_reportLen << "\n";
    os << "device_unique_data = [";
    for (int i = 0; i < 12; ++i) {
        if (i) os << ", ";
        os << dud[i];
    }
    os << "]\n";
    // Only emit clear_dud0_when_raw1_bits when Start/Select share the byte
    // that carries the fret bitmap (PS3 GH: both at byte 1, fret at byte 0).
    // On PS4/PS5 layouts the fret bitmap and menu buttons live in different
    // bytes so the suppression isn't needed.
    if (selByte == 1 && staByte == 1) {
        os << "clear_dud0_when_raw1_bits = 0x" << std::hex
           << int(b_sel | b_sta) << std::dec << "\n";
    }

    // Detect which raw byte holds the fret bitmap (PS3: byte 0; PS4 RB: 46;
    // PS5 Riffmaster: 43) and whether the bit order needs remapping to the
    // PS4-native (G=0, R=1, Y=2, B=3, O=4) layout RB4 reads.
    int fretByte = -1;
    int remap[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    bool needs_remap = false;
    if (m_deviceType == DeviceType::Guitar) {
        struct FretMap { const char* step; int ps4_bit; };
        const FretMap frets[] = {
            {"green_fret",  0},
            {"red_fret",    1},
            {"yellow_fret", 2},
            {"blue_fret",   3},
            {"orange_fret", 4},
        };
        uint8_t fretMaskBits = 0;
        for (const auto& fm : frets) {
            auto [byte, mask] = detectFlagByteAndBit(fm.step);
            if (byte < 0 || mask == 0) continue;
            if (fretByte < 0) fretByte = byte;
            // Only contribute to the mask if the fret was found on the same
            // byte as the others — different bytes mean a mixed layout the
            // single fret_mask can't capture.
            if (byte == fretByte) fretMaskBits |= mask;
            for (int b = 0; b < 8; ++b) {
                if (mask & (1 << b)) {
                    if (b != fm.ps4_bit) needs_remap = true;
                    remap[b] = fm.ps4_bit;
                    break;
                }
            }
        }
        if (needs_remap) {
            os << "dud0_bit_remap = [";
            for (int i = 0; i < 8; ++i) {
                if (i) os << ", ";
                os << remap[i];
            }
            os << "]\n";
        }
        // Emit fret_mask when only some bits of fretByte hold fret data
        // (PS4 RB / PS5 Riffmaster share the byte with HAT in the low nibble).
        if (fretMaskBits != 0 && fretMaskBits != 0xFF) {
            os << "fret_mask = 0x" << std::hex << int(fretMaskBits) << std::dec << "\n";
        }
    }
    os << "hat_byte = " << hatByte << "\n";
    if (m_deviceType == DeviceType::ProDrum) {
        os << "drum_ps4_layout = true\n";
        const int red_b    = velByte("red_pad");
        const int blue_b   = velByte("blue_pad");
        const int yellow_b = velByte("yellow_pad");
        const int green_b  = velByte("green_pad");
        const int y_cym    = velByte("yellow_cymbal");
        const int b_cym    = velByte("blue_cymbal");
        const int g_cym    = velByte("green_cymbal");
        const int o_cym    = velByte("orange_cymbal");
        os << "drum_red_byte           = " << red_b    << "\n";
        os << "drum_blue_byte          = " << blue_b   << "\n";
        os << "drum_yellow_byte        = " << (yellow_b >= 0 ? yellow_b : y_cym) << "\n";
        os << "drum_green_byte         = " << green_b  << "\n";
        os << "drum_yellow_cymbal_byte = " << (y_cym >= 0 ? y_cym : yellow_b) << "\n";
        os << "drum_blue_cymbal_byte   = " << b_cym    << "\n";
        os << "drum_green_cymbal_byte  = " << (g_cym >= 0 ? g_cym : o_cym) << "\n";
    }
    if (m_deviceType == DeviceType::Guitar) {
        os << "guitar_ps4_layout = true\n";
        if (fretByte >= 0) os << "fret_byte = " << fretByte << "\n";
        const int whammy = velByte("whammy_bar");
        const int touch  = velByte("touch_slider");
        if (whammy >= 0) {
            os << "whammy_byte = " << whammy << "\n";
            // PS3 GH guitars idle at 0x80 (centered axis); PS4 RB / PS5
            // Riffmaster idle at 0x00. Use the captured baseline to decide.
            const int wb = m_baselineMax[whammy];
            if (wb < 0x40) os << "whammy_baseline = 0\n";
        }
        if (touch  >= 0) os << "touch_byte  = " << touch  << "\n";
        if (touch  >= 0) os << "tone_byte   = " << touch  << "\n";
    }
    if (m_deviceType == DeviceType::Guitar && !m_motionBytes.empty()) {
        const int tilt = *m_motionBytes.begin();
        os << "tilt_byte      = " << tilt << "\n";
        const bool has_high = m_motionBytes.count(tilt + 1) > 0;
        const int baseline = (m_baselineMin[tilt] + m_baselineMax[tilt]) / 2;
        // Compare the tilt byte while the user actively lifts the guitar
        // (the dedicated "tilt_up" step) against its idle midpoint, so we
        // know whether raw INCREASES or DECREASES when pointed up. Works
        // regardless of accel polarity (PS3 GH: lower = up; PS5: higher = up).
        int up_min = baseline, up_max = baseline;
        for (const auto& r : m_results) {
            if (r.def.key != QStringLiteral("tilt_up") || !r.captured) continue;
            if (tilt < 0 || tilt >= m_reportLen) break;
            up_min = r.bytes[tilt].min;
            up_max = r.bytes[tilt].max;
            break;
        }
        const int up_delta_high = up_max - baseline;
        const int up_delta_low  = baseline - up_min;
        const bool invert = up_delta_high > up_delta_low;
        if (has_high) {
            os << "tilt_byte_high = " << (tilt + 1) << "\n";
            os << "tilt_baseline  = 512\n";
            os << "tilt_scale     = 128\n";
        } else {
            os << "tilt_baseline  = " << baseline << "\n";
            os << "tilt_scale     = 80\n";
        }
        os << "tilt_invert    = " << (invert ? "true" : "false") << "\n";
    }
    if (!m_motionBytes.empty()) {
        os << "motion_bytes = [";
        bool first = true;
        for (int b : m_motionBytes) { if (!first) os << ", "; os << b; first = false; }
        os << "]\n";
    }
    // Fold Start/Select into the per-byte map so each TOML section gets
    // emitted exactly once, even when the kit puts menu buttons in the
    // same byte as the face buttons (or a totally different byte on PS4/PS5).
    const char* selName = (m_deviceType == DeviceType::Guitar) ? "left" : "touchpad";
    const char* selOrigin = (m_deviceType == DeviceType::Guitar)
        ? "Select (Star Power)" : "Select";
    if (b_sel) button_bits.push_back({selByte, b_sel, selName, selOrigin});
    if (b_sta) button_bits.push_back({staByte, b_sta, "options", "Start"});

    std::map<int, std::vector<ButtonBit>> by_byte;
    for (const auto& b : button_bits) {
        if (b.mask != 0) by_byte[b.byte].push_back(b);
    }
    auto emit_bit = [&](uint8_t bit, const char* name, const char* origin) {
        os << "\"0x" << std::hex;
        if (bit < 0x10) os << "0";
        os << int(bit) << std::dec << "\" = \"" << name << "\"";
        if (origin && *origin) os << "  # " << origin;
        os << '\n';
    };
    for (const auto& [byte_idx, bits] : by_byte) {
        os << "\n[buttons_byte_" << byte_idx << "]\n";
        // Same bit can be claimed by multiple inputs (e.g. drum kick + orange
        // both on L1); de-dupe so we don't emit duplicate TOML keys.
        std::map<uint8_t, std::pair<const char*, const char*>> uniq;
        for (const auto& b : bits) {
            uniq.try_emplace(b.mask, std::make_pair(b.name, b.origin));
        }
        for (const auto& [mask, name_origin] : uniq) {
            emit_bit(mask, name_origin.first, name_origin.second);
        }
    }

    bool emitted_scaling_header = false;
    for (const auto& s : scalePlan) {
        int bestByte = -1, bestRange = 0;
        for (const auto& r : m_results) {
            if (r.def.key != s.stepKey || !r.captured) continue;
            for (int i = 3; i < m_reportLen; ++i) {
                if (m_motionBytes.count(i)) continue;
                const auto& b = r.bytes[i];
                if (b.samples == 0) continue;
                const int range = b.max - b.min;
                if (range > bestRange && b.max >= 0x10 && i != 26) {
                    bestRange = range; bestByte = i;
                }
            }
        }
        if (bestByte < 0) continue;
        int lo = -1, hi = -1;
        for (const auto& r : m_results) {
            if (r.def.key != s.stepKey || !r.captured) continue;
            lo = std::max(0, m_baselineMax[bestByte]) + 2;
            hi = r.bytes[bestByte].max;
            break;
        }
        if (hi <= lo) continue;
        if (!emitted_scaling_header) {
            os << "\n[velocity_scaling]\n";
            emitted_scaling_header = true;
        }
        os << '"' << s.dudIdx << "\" = { lo = " << lo << ", hi = " << hi
           << " }  # " << s.comment << '\n';
    }

    return QString::fromStdString(os.str());
}

void KitProbeDialog::onSaveResults() {
    // Always write into <user>/kits/ so the C++ runtime loader picks the
    // new kit up on next launch. No file picker — the filename is derived
    // from the VID:PID so subsequent re-probes of the same kit overwrite
    // cleanly.
    namespace fs = std::filesystem;
    fs::path dir;
    try {
        dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "kits";
        fs::create_directories(dir);
    } catch (const std::exception& e) {
        QMessageBox::warning(this, tr("Save failed"),
                             tr("Could not access user kits directory: %1")
                                 .arg(QString::fromUtf8(e.what())));
        return;
    }

    const QString base = QStringLiteral("kit_%1_%2")
        .arg(m_vid, 4, 16, QChar('0'))
        .arg(m_pid, 4, 16, QChar('0'));
    const QString tomlPath = QString::fromStdString((dir / (base.toStdString() + ".toml")).string());
    const QString jsonPath = QString::fromStdString((dir / (base.toStdString() + ".json")).string());
    const QString rawPath  = QString::fromStdString((dir / (base.toStdString() + ".raw.jsonl")).string());

    QFile f(tomlPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Save failed"), f.errorString());
        return;
    }
    f.write(deriveKitToml().toUtf8());
    f.close();

    QFile jf(jsonPath);
    if (jf.open(QIODevice::WriteOnly | QIODevice::Text)) {
        jf.write(deriveCalibrationJson().toUtf8());
        jf.close();
    }

    QFile rf(rawPath);
    if (rf.open(QIODevice::WriteOnly | QIODevice::Text)) {
        for (const auto& step : m_results) {
            if (!step.captured) continue;
            for (const auto& report : step.raw) {
                QString line = QStringLiteral("{\"step\":\"%1\",\"bytes\":[")
                                   .arg(step.def.key);
                for (std::size_t i = 0; i < report.size(); ++i) {
                    if (i) line += ',';
                    line += QString::number(report[i]);
                }
                line += "]}\n";
                rf.write(line.toUtf8());
            }
        }
        rf.close();
    }

    QMessageBox::information(this, tr("Saved"),
        tr("Saved into your shadPS4 user folder:\n\n"
           "  %1   (runtime kit definition, auto-loaded at next launch)\n"
           "  %2   (calibration record, share on Discord)\n"
           "  %3   (raw HID captures, for re-deriving the mapping later)")
            .arg(tomlPath).arg(jsonPath).arg(rawPath));
}
