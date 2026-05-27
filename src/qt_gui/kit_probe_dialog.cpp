// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "kit_probe_dialog.h"
#include "ui_kit_probe_dialog.h"

#include <QBrush>
#include <QColor>
#include <QDateTime>
#include <QCryptographicHash>
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
#include "input/hid_kit_probe_data.h"

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
        {"orange_pad",      QObject::tr("Orange pad (5-lane GH kits only — skip if absent)"),  "velocity", true},
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
        {"green_strum",    QObject::tr("Hold GREEN and strum DOWN at the same time"),  "combo",    false},
        {"green_blue",     QObject::tr("Hold GREEN and BLUE together"),                "combo",    false},
        {"green_blue_strum", QObject::tr("Hold GREEN + BLUE and strum DOWN"),          "combo",    false},
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

// Solo-fret guitar = standard guitar walkthrough + 5 upper-neck solo frets.
// PS4 RB Mustang / PS5 Riffmaster have these; PS3 GH/RB and most XInput
// guitars do not. Picked from the dropdown so users who lack them don't
// have to step through "skip" five extra times.
const std::vector<KitProbeDialog::StepDef>& GuitarSoloSteps() {
    static const std::vector<KitProbeDialog::StepDef> steps = [] {
        std::vector<KitProbeDialog::StepDef> v = GuitarSteps();
        auto insertAt = v.begin();
        for (auto it = v.begin(); it != v.end(); ++it) {
            if (it->key == "strum_up") { insertAt = it; break; }
        }
        v.insert(insertAt, {
            {"solo_green_fret",  QObject::tr("UPPER solo GREEN fret — hold and release"),    "digital", false},
            {"solo_red_fret",    QObject::tr("UPPER solo RED fret"),                          "digital", false},
            {"solo_yellow_fret", QObject::tr("UPPER solo YELLOW fret"),                       "digital", false},
            {"solo_blue_fret",   QObject::tr("UPPER solo BLUE fret"),                         "digital", false},
            {"solo_orange_fret", QObject::tr("UPPER solo ORANGE fret"),                       "digital", false},
        });
        return v;
    }();
    return steps;
}

const std::vector<KitProbeDialog::StepDef>& Steps(KitProbeDialog::DeviceType t) {
    // Drum and ProDrum share the same input step list — Pro mode just emits
    // a different TOML layout downstream (separate cymbal slots).
    using DT = KitProbeDialog::DeviceType;
    switch (t) {
    case DT::Guitar:     return GuitarSteps();
    case DT::GuitarSolo: return GuitarSoloSteps();
    default:             return DrumSteps();
    }
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
    // Index 0 is the "— pick instrument type —" placeholder. Force the user
    // to pick one before the walkthrough starts, so we don't silently
    // capture a guitar fixture as if it were a drum kit.
    switch (ui->deviceTypeCombo->currentIndex()) {
        case 1:  m_deviceType = DeviceType::Drum;       break;  // 5-lane / no cymbals
        case 2:  m_deviceType = DeviceType::ProDrum;    break;  // Pro drums
        case 3:  m_deviceType = DeviceType::Guitar;     break;
        case 4:  m_deviceType = DeviceType::GuitarSolo; break;  // PS4/PS5 RB solo frets
        default:
            QMessageBox::information(
                this, tr("Pick an instrument type"),
                tr("Pick an instrument type from the dropdown on the left "
                   "before starting the probe."));
            closeDevice();
            setState(State::SelectDevice);
            return;
    }
    for (const auto& s : Steps(m_deviceType)) m_results.push_back({s, {}, {}, false});
    m_currentStep = -1;
    m_idleRaw.clear();
    std::fill(m_baselineMax.begin(), m_baselineMax.end(), 0);
    std::fill(m_baselineMin.begin(), m_baselineMin.end(), 0xFF);
    m_motionBytes.clear();
    resetByteGrid(m_reportLen);

    // Baseline: 1.5s of quiet sampling so we know idle noise band.
    ui->stepHeader->setText(tr("Baseline (1/%1)").arg(Steps(m_deviceType).size() + 1));
    ui->stepPrompt->setText(
        tr("Don't touch the kit. Capturing idle noise for ~1.5 s...\n\n"
           "If your kit only sends data when something changes (custom "
           "firmware like Santroller, some wireless kits), tap and release "
           "any button now so we can read its idle state."));
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
        // Keep updating the baseline band as long as the kit is in the Idle
        // phase. This covers Santroller-style kits that only emit HID reports
        // on state change, so the 1.5 s idle window may capture nothing
        // until the user presses a button.
        if (m_state == State::Idle) {
            m_idleRaw.emplace_back(buf, buf + n);
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
        // Keep updating the baseline band as long as the kit is in the Idle
        // phase. This covers Santroller-style kits that only emit HID reports
        // on state change, so the 1.5 s idle window may capture nothing
        // until the user presses a button.
        if (m_state == State::Idle) {
            m_idleRaw.emplace_back(buf, buf + n);
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
            // A "silent" device (Santroller-style firmware, some wireless
            // kits) only sends a report on state change. If we got nothing
            // through the 1.5 s baseline, every byte still holds the
            // initial baseline_min=0xFF / baseline_max=0 sentinels. Warn
            // the user instead of silently continuing with all-zeros
            // baseline data — the wizard's per-step transition detection
            // misfires when baseline is wrong.
            const bool received_anything = std::any_of(
                m_baselineMin.begin(),
                m_baselineMin.begin() + m_reportLen,
                [](int v) { return v != 0xFF; });
            if (!received_anything) {
                ui->stepPrompt->setText(
                    tr("⚠ No data received during baseline. Hold any "
                       "button now to wake the kit, then release. The "
                       "wizard will continue once it sees its first "
                       "report."));
            } else {
                ui->stepPrompt->setText(
                    tr("Baseline captured. Click Next to begin the walk-through."));
                ui->nextBtn->setEnabled(true);
            }
        }
    } else if (m_state == State::Step && m_sampling) {
        ui->stepProgress->setValue(std::min(elapsed, kStepDurationMs));
        if (elapsed >= kStepDurationMs) {
            finishStep();
        }
    }
}

// ============================================================================
// Output: TOML (runtime kit def). Raw HID captures are streamed to .raw.jsonl
// during save (see onSaveResults).
// ============================================================================

QString KitProbeDialog::deriveKitToml() const {
    // Convert wizard state into the Qt-free KitProbeData snapshot and call
    // the shared deriver. The same function runs in tests against
    // .raw.jsonl captures, so any TOML-shape bug shows up before shipping.
    using Input::HidInstrument::KitProbeData;
    using Input::HidInstrument::StepResultData;
    using Input::HidInstrument::ProbeDeviceType;

    KitProbeData data;
    data.vid = m_vid;
    data.pid = m_pid;
    data.device_name = m_deviceName.toStdString();
    data.is_xinput = m_isXInput;
    data.report_length = m_reportLen;
    switch (m_deviceType) {
    case DeviceType::Drum:       data.device_type = ProbeDeviceType::Drum;       break;
    case DeviceType::ProDrum:    data.device_type = ProbeDeviceType::ProDrum;    break;
    case DeviceType::Guitar:     data.device_type = ProbeDeviceType::Guitar;     break;
    case DeviceType::GuitarSolo: data.device_type = ProbeDeviceType::GuitarSolo; break;
    }
    data.results.reserve(m_results.size() + 1);

    StepResultData idle_res;
    idle_res.key = "_idle_baseline";
    idle_res.kind = "baseline";
    idle_res.captured = true;
    idle_res.raw = m_idleRaw;
    for (int i = 0; i < 64; ++i) idle_res.bytes[i].min = 0xFF;
    for (const auto& raw_bytes : m_idleRaw) {
        for (std::size_t i = 0; i < raw_bytes.size() && i < 64; ++i) {
            idle_res.bytes[i].max = std::max<int>(idle_res.bytes[i].max, raw_bytes[i]);
            idle_res.bytes[i].min = std::min<int>(idle_res.bytes[i].min, raw_bytes[i]);
            if (raw_bytes[i] != 0 && (idle_res.bytes[i].min_nonzero < 0 || raw_bytes[i] < idle_res.bytes[i].min_nonzero))
                idle_res.bytes[i].min_nonzero = raw_bytes[i];
            idle_res.bytes[i].samples++;
        }
    }
    data.results.push_back(std::move(idle_res));

    for (const auto& r : m_results) {
        StepResultData out;
        out.key = r.def.key.toStdString();
        out.kind = r.def.kind.toStdString();
        out.captured = r.captured;
        for (int i = 0; i < 64; ++i) {
            out.bytes[i].min = r.bytes[i].min;
            out.bytes[i].max = r.bytes[i].max;
            out.bytes[i].min_nonzero = r.bytes[i].min_nonzero;
            out.bytes[i].transitions = r.bytes[i].transitions;
            out.bytes[i].samples = r.bytes[i].samples;
        }
        out.raw = r.raw;
        data.results.push_back(std::move(out));
    }

    Input::HidInstrument::DeriveBaselineAndMotion(data);
    return QString::fromStdString(Input::HidInstrument::DeriveKitToml(data));
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

    // Two physical devices can share a VID:PID (Santroller flashed as a GH5
    // clone vs. as a Pro Drum, for instance). Include a short SHA-1 of the
    // device name in the filename so the second capture doesn't overwrite
    // the first when the user re-runs the wizard for a different kit.
    const QByteArray name_hash = QCryptographicHash::hash(
        m_deviceName.toUtf8(), QCryptographicHash::Sha1).toHex().left(8);
    const QString base = QStringLiteral("kit_%1_%2_%3")
        .arg(m_vid, 4, 16, QChar('0'))
        .arg(m_pid, 4, 16, QChar('0'))
        .arg(QString::fromLatin1(name_hash));
    const QString tomlPath = QString::fromStdString((dir / (base.toStdString() + ".toml")).string());
    const QString rawPath  = QString::fromStdString((dir / (base.toStdString() + ".raw.jsonl")).string());

    QFile f(tomlPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Save failed"), f.errorString());
        return;
    }
    f.write(deriveKitToml().toUtf8());
    f.close();

    // Raw HID capture file. First line is a "meta" record with provenance
    // (format version, VID:PID, device name, report length, ISO timestamp);
    // subsequent lines are {"step":"name","bytes":[...]} samples. The test
    // harness uses the meta to re-derive a TOML and replay scenarios.
    QFile rf(rawPath);
    if (rf.open(QIODevice::WriteOnly | QIODevice::Text)) {
        const char* deviceTypeStr = "guitar";
        switch (m_deviceType) {
        case DeviceType::Drum:       deviceTypeStr = "drum"; break;
        case DeviceType::ProDrum:    deviceTypeStr = "drum_pro"; break;
        case DeviceType::Guitar:     deviceTypeStr = "guitar"; break;
        case DeviceType::GuitarSolo: deviceTypeStr = "guitar_solo"; break;
        }
        // "source" distinguishes raw HID dumps from SDL_GameController/XInput
        // taps. The test harness uses it to decide whether report[0] is a HID
        // report-ID or already the first data byte.
        const char* sourceStr = m_isXInput ? "xinput" : "hid";
        QString meta = QStringLiteral(
            "{\"type\":\"meta\",\"version\":4,"
            "\"vendor_id\":\"0x%1\",\"product_id\":\"0x%2\","
            "\"device_name\":\"%3\",\"device_type\":\"%4\","
            "\"source\":\"%5\","
            "\"report_length\":%6,\"timestamp\":\"%7\"}\n")
            .arg(m_vid, 4, 16, QChar('0'))
            .arg(m_pid, 4, 16, QChar('0'))
            .arg(QString(m_deviceName).replace('"', '\''))
            .arg(QString::fromLatin1(deviceTypeStr))
            .arg(QString::fromLatin1(sourceStr))
            .arg(m_reportLen)
            .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        rf.write(meta.toUtf8());
        for (const auto& report : m_idleRaw) {
            QString line = QStringLiteral("{\"step\":\"_idle_baseline\",\"bytes\":[");
            for (std::size_t i = 0; i < report.size(); ++i) {
                if (i) line += ',';
                line += QString::number(report[i]);
            }
            line += "]}\n";
            rf.write(line.toUtf8());
        }
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
           "  %2   (raw HID captures, for re-deriving the mapping later)")
            .arg(tomlPath).arg(rawPath));
}
