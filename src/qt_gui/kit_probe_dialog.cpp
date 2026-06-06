// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "kit_probe_dialog.h"
#include "ui_kit_probe_dialog.h"

#include <QBrush>
#include <QColor>
#include <QDateTime>
#include <QCryptographicHash>
#include <QSysInfo>
#include <QUuid>
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
#include "input/midi_input.h"
#include "input/midi_kit_probe_data.h"
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
        {"button_start",    QObject::tr("Start button (tap once or twice)"),                    "digital",  false, 3000},
        {"button_select",   QObject::tr("Select button (tap once or twice)"),                   "digital",  false, 3000},
        {"button_ps",       QObject::tr("PS / Home button (skip if absent)"),                   "digital",  true,  3000},
        {"button_square",   QObject::tr("Square face button"),                                  "digital",  true,  3000},
        {"button_cross",    QObject::tr("Cross / X face button"),                               "digital",  true,  3000},
        {"button_circle",   QObject::tr("Circle face button"),                                  "digital",  true,  3000},
        {"button_triangle", QObject::tr("Triangle face button"),                                "digital",  true,  3000},
        {"dpad_up",         QObject::tr("D-pad UP (hold briefly)"),                             "digital",  false, 3000},
        {"dpad_down",       QObject::tr("D-pad DOWN (hold briefly)"),                           "digital",  false, 3000},
        {"dpad_left",       QObject::tr("D-pad LEFT (hold briefly)"),                           "digital",  false, 3000},
        {"dpad_right",      QObject::tr("D-pad RIGHT (hold briefly)"),                          "digital",  false, 3000},
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
        {"button_start",   QObject::tr("Start button (tap once or twice)"),                "digital",  false, 3000},
        {"button_select",  QObject::tr("Select button (tap once or twice)"),               "digital",  false, 3000},
        {"button_ps",      QObject::tr("PS / Home button (skip if absent)"),               "digital",  true,  3000},
        // dpad_up / dpad_down deliberately omitted — guitar strum bars
        // wire the same HAT bits as dpad up/down, and the strum_up/
        // strum_down steps already capture them. Adding redundant dpad
        // up/down steps only created cross-talk warnings on devices
        // where the strum bar slightly bounced the dpad bit pattern.
        {"dpad_left",      QObject::tr("D-pad LEFT (skip if absent)"),                     "digital",  true,  3000},
        {"dpad_right",     QObject::tr("D-pad RIGHT (skip if absent)"),                    "digital",  true,  3000},
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
        // PS4 RB Mustang / PS5 Riffmaster don't have a continuous touch
        // strip — they ship a pickup-switch instead (already captured as
        // `fx_switch`). Skip the touch_slider step for this device type so
        // users don't waste a slot on something that doesn't exist on
        // their kit.
        v.erase(std::remove_if(v.begin(), v.end(),
                               [](const KitProbeDialog::StepDef& s) {
                                   return s.key == "touch_slider";
                               }),
                v.end());
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
            // Solo-frets combo — analogue of green_blue for the main
            // neck. Catches kits where solo bits land on the same byte
            // but a single press fires multiple bits (broken bit mask)
            // or where two simultaneous solo bits get swallowed
            // (button-bytes section missing).
            {"solo_green_blue",  QObject::tr("Hold UPPER solo GREEN and UPPER solo BLUE together"), "combo", false},
            // PS4 RB Mustang / PS5 Riffmaster / X360 RB all ship a
            // discrete pickup/FX switch (the only guitars that DO);
            // capture it here instead of in the standard walkthrough.
            // Optional so kits with a broken or missing switch can move
            // on — the rest of the kit still functions without it.
            {"fx_switch",        QObject::tr("FX / pickup switch — sweep through every position (skip if absent)"), "velocity", true},
        });
        return v;
    }();
    return steps;
}

const std::vector<KitProbeDialog::StepDef>& ProDrumSteps() {
    static const std::vector<KitProbeDialog::StepDef> steps = [] {
        std::vector<KitProbeDialog::StepDef> v = DrumSteps();
        auto insertAt = v.begin();
        for (auto it = v.begin(); it != v.end(); ++it) {
            if (it->key == "button_start") { insertAt = it; break; }
        }
        v.insert(insertAt, {
            {"yellow_cymbal",   QObject::tr("Yellow cymbal"),                                       "velocity", true},
            {"orange_cymbal",   QObject::tr("Orange cymbal (skip if absent)"),                      "velocity", true},
            {"blue_cymbal",     QObject::tr("Blue cymbal (skip if absent)"),                       "velocity", true},
            {"green_cymbal",    QObject::tr("Green cymbal (skip if absent)"),                      "velocity", true},
        });
        return v;
    }();
    return steps;
}

const std::vector<KitProbeDialog::StepDef>& Steps(KitProbeDialog::DeviceType t) {
    using DT = KitProbeDialog::DeviceType;
    switch (t) {
    case DT::Guitar:     return GuitarSteps();
    case DT::GuitarSolo: return GuitarSoloSteps();
    case DT::ProDrum:    return ProDrumSteps();
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
    // MIDI input ports. Path uses "midi:<id>" so openDevice can route to MidiInput.
    for (const auto& port : Input::MidiInput::EnumerateInputPorts()) {
        const QString port_id = QString::fromStdString(port.id);
        const QString port_name = QString::fromStdString(port.name);
        const QString label = QStringLiteral("[MIDI] %1  %2").arg(port_id, port_name);
        auto* item = new QListWidgetItem(label, ui->deviceList);
        item->setData(Qt::UserRole + 0, QStringLiteral("midi:%1").arg(port_id));
        item->setData(Qt::UserRole + 1, QStringLiteral("0000"));
        item->setData(Qt::UserRole + 2, QStringLiteral("0000"));
        item->setData(Qt::UserRole + 3, port_name);
        item->setData(Qt::UserRole + 4, false);
        item->setData(Qt::UserRole + 5, QStringLiteral("midi"));  // source kind marker
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
    m_isMidi = path.startsWith(QStringLiteral("midi:"));
    if (m_isMidi) {
        const QString port_id = path.mid(static_cast<int>(std::string_view("midi:").size()));
        m_midiDev = Input::MidiInput::OpenInputPort(port_id.toStdString());
        if (!m_midiDev) {
            QMessageBox::warning(this, tr("Open failed"),
                tr("Could not open MIDI port %1.").arg(port_id));
            return false;
        }
        m_devicePath = path;
        m_deviceName = name;
        m_vid = 0;
        m_pid = 0;
        m_midiPortId = port_id;
        resetByteGrid(0);
        return true;
    }
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
    if (m_midiDev) {
        Input::MidiInput::CloseInputPort(m_midiDev);
        m_midiDev = nullptr;
    }
    m_devicePath.clear();
    m_deviceName.clear();
    m_vid = m_pid = 0;
    m_isXInput = false;
    m_isMidi = false;
    m_midiPortId.clear();
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
    for (const auto& s : Steps(m_deviceType)) {
        // MIDI drum kits / Python senders / electronic kits don't have
        // dpad/face-button hardware — every channel message is a
        // note-on/off, mapped to a pad. Force-walking the user through
        // 11 button steps that can't possibly fire is pointless and
        // makes the wizard feel broken on MIDI. Skip every digital /
        // combo step for MIDI captures; the velocity (pad) and tilt
        // motion steps are still kept because some e-drum modules
        // emit those as continuous controllers.
        if (m_isMidi && (s.kind == "digital" || s.kind == "combo")) continue;
        m_results.push_back({s, {}, {}, false});
    }
    // Parallel MIDI event buffer — same length as m_results so indices
    // line up. Stays unused for HID/XInput captures.
    m_midi_results.assign(m_results.size(), MidiStepBuf{});
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
    // Reset the parallel MIDI buffer too; harmless when not in MIDI mode.
    if (idx < (int)m_midi_results.size()) {
        m_midi_results[idx].events.clear();
        m_midi_results[idx].captured = false;
        m_midi_results[idx].step_started = std::chrono::steady_clock::now();
    }
    ui->stepHeader->setText(tr("Step %1/%2 — %3")
                                .arg(idx + 2)
                                .arg(Steps(m_deviceType).size() + 1)
                                .arg(r.def.key));
    const int dur_ms = r.def.duration_ms > 0 ? r.def.duration_ms : kStepDurationMs;
    const int dur_s = (dur_ms + 500) / 1000;
    ui->stepPrompt->setText(
        r.def.prompt +
        tr("\n\nRead the prompt, get ready, then click \"Begin sampling\". "
           "You'll have %1 s to wail on it lots of ways. Click Begin again "
           "to redo if needed.").arg(dur_s));
    ui->stepProgress->setMaximum(dur_ms);
    ui->stepProgress->setValue(0);
    ui->crossTalkLabel->clear();
    ui->beginBtn->setText(tr("Begin sampling (%1 s)").arg(dur_s));
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
    if (m_currentStep < (int)m_midi_results.size()) {
        m_midi_results[m_currentStep].captured = true;
    }
    m_sampling = false;
    ui->stepProgress->setValue(ui->stepProgress->maximum());
    ui->beginBtn->setText(tr("Redo this step"));
    ui->beginBtn->setEnabled(true);
    ui->nextBtn->setEnabled(true);
    ui->skipBtn->setEnabled(false);
    // Cross-talk detection is byte-grid based, irrelevant to MIDI captures.
    if (!m_isMidi) detectCrossTalkAndWarn();
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
                if ((b.max - b.min) <= 3) continue;
                // A byte that only flips one or two bits across the whole
                // motion-baseline step is a tilt FLAG or button bit that
                // happened to fire while the user was tilting — not an
                // accelerometer. Flagging it as motion permanently excludes
                // it from fret/face-button detection in the remaining
                // steps, which kills the kit. PS3 GH guitars (incl. the
                // CRKD in PS3 mode) put the tilt flag on bit 5 of byte 0,
                // the same byte that holds the fret bitmap, so falsely
                // flagging byte 0 as motion silently drops every fret.
                // True accelerometer bytes flip many bits as they sweep
                // 0..0xFF; flag-bytes touch <= 2.
                const int diff = (b.max ^ b.min) & 0xFF;
                const int popcount = __builtin_popcount(static_cast<unsigned>(diff));
                if (popcount <= 2) continue;
                m_motionBytes.insert(i);
                ++counted;
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
    if (m_midiDev) {
        auto events = Input::MidiInput::DrainEvents(m_midiDev);
        constexpr int kFirstNote = 30;
        constexpr int kNoteCount = 64;
        for (const auto& ev : events) {
            const int col = static_cast<int>(ev.note) - kFirstNote;
            if (col < 0 || col >= kNoteCount) continue;
            auto* cell = ui->byteGrid->item(1, col);
            if (!cell) continue;
            if (ev.on) {
                cell->setText(QString::number(ev.velocity));
                cell->setForeground(QBrush(QColor("#fff")));
                cell->setBackground(QBrush(QColor("#5a7a3a")));
            } else {
                cell->setBackground(QBrush(QColor("#3a4a2a")));
            }
        }
        if (m_state == State::Idle && !events.empty()) {
            ui->stepPrompt->setText(
                tr("MIDI input detected (%1 event%2). Click Next to begin "
                   "the walk-through.")
                    .arg(events.size())
                    .arg(events.size() == 1 ? "" : "s"));
            ui->nextBtn->setEnabled(true);
            ui->stepProgress->setValue(ui->stepProgress->maximum());
            return;
        }
        if (m_state == State::Step && m_sampling &&
            m_currentStep >= 0 && m_currentStep < (int)m_midi_results.size()) {
            auto& buf = m_midi_results[m_currentStep];
            const auto step_start = buf.step_started;
            for (const auto& ev : events) {
                MidiStepEvent rec;
                rec.on = ev.on;
                rec.note = ev.note;
                rec.velocity = ev.velocity;
                // Recompute the timestamp relative to the step (not the
                // port open) so the capture file's t_ms is monotonic
                // from 0 within each step — same convention HID
                // captures use implicitly via their per-step framing.
                rec.t_ms = static_cast<std::uint32_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - step_start)
                        .count()); // relative to step start, matching HID framing convention
                buf.events.push_back(rec);
            }
        }
        return;
    }
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
    if (m_isMidi) {
        constexpr int kFirstNote = 30;
        constexpr int kNoteCount = 64;
        g->setColumnCount(kNoteCount);
        g->setRowCount(2);
        for (int i = 0; i < kNoteCount; ++i) {
            auto* hdr = new QTableWidgetItem(QString::number(kFirstNote + i));
            hdr->setFlags(Qt::ItemIsEnabled);
            hdr->setForeground(QBrush(QColor("#888")));
            g->setItem(0, i, hdr);
            auto* cell = new QTableWidgetItem("--");
            cell->setFlags(Qt::ItemIsEnabled);
            cell->setForeground(QBrush(QColor("#666")));
            g->setItem(1, i, cell);
            g->setColumnWidth(i, 28);
        }
        g->resizeRowsToContents();
        return;
    }
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
    onHidReadable();

    using clock = std::chrono::steady_clock;
    const int elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            clock::now() - m_stepStart).count();
    if (m_state == State::Idle) {
        ui->stepProgress->setValue(std::min(elapsed, kBaselineDurationMs));
        if (elapsed >= kBaselineDurationMs) {
            if (m_isMidi) {
                // MIDI is event-based — an idle kit produces zero events, which is normal.
                ui->stepPrompt->setText(
                    tr("Baseline captured (MIDI idle). Click Next to begin the walk-through."));
                ui->nextBtn->setEnabled(true);
            } else {
                // Santroller-style firmware only sends on state change — if baseline is
                // empty the sentinels (min=0xFF, max=0) will misfire the step detector.
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
        }
    } else if (m_state == State::Step && m_sampling) {
        const int dur_ms = (m_currentStep >= 0 &&
                            m_results[m_currentStep].def.duration_ms > 0)
                               ? m_results[m_currentStep].def.duration_ms
                               : kStepDurationMs;
        ui->stepProgress->setValue(std::min(elapsed, dur_ms));
        if (elapsed >= dur_ms) {
            finishStep();
        }
    }
}

// ============================================================================
// Output: TOML (runtime kit def). Raw HID captures are streamed to .raw.jsonl
// during save (see onSaveResults).
// ============================================================================

QString KitProbeDialog::deriveKitToml() const {
    if (m_isMidi) {
        using namespace Input::MidiInstrument;
        MidiKitProbeData md;
        md.device_name = m_deviceName.toStdString();
        md.port_id = m_midiPortId.toStdString();
        md.device_type = (m_deviceType == DeviceType::ProDrum)
                             ? MidiDeviceType::ProDrum
                             : MidiDeviceType::Drum;
        md.results.reserve(m_midi_results.size());
        for (std::size_t i = 0; i < m_midi_results.size(); ++i) {
            const auto& src = m_midi_results[i];
            MidiStepResult dst;
            dst.key = m_results[i].def.key.toStdString();
            dst.captured = src.captured;
            dst.events.reserve(src.events.size());
            for (const auto& ev : src.events) {
                MidiEvent e;
                e.on = ev.on;
                e.note = ev.note;
                e.velocity = ev.velocity;
                e.t_ms = ev.t_ms;
                dst.events.push_back(e);
            }
            md.results.push_back(std::move(dst));
        }
        return QString::fromStdString(DeriveMidiKitToml(md));
    }
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

    if (m_isMidi) {
        // Filename: midi_<slug>_<hash>  (no VID:PID for MIDI; slug from device name)
        QString slug = m_deviceName.toLower();
        for (QChar& c : slug)
            if (!c.isLetterOrNumber()) c = '_';
        while (slug.contains(QStringLiteral("__")))
            slug.replace(QStringLiteral("__"), QStringLiteral("_"));
        if (slug.startsWith('_')) slug.remove(0, 1);
        if (slug.endsWith('_')) slug.chop(1);
        if (slug.size() > 32) slug.truncate(32);
        if (slug.isEmpty()) slug = QStringLiteral("midi");
        const QByteArray name_hash = QCryptographicHash::hash(
            (m_deviceName + m_midiPortId).toUtf8(),
            QCryptographicHash::Sha1).toHex().left(8);
        const QString base = QStringLiteral("midi_%1_%2")
                                  .arg(slug)
                                  .arg(QString::fromLatin1(name_hash));
        const QString tomlPath = QString::fromStdString(
            (dir / (base.toStdString() + ".toml")).string());
        const QString rawPath = QString::fromStdString(
            (dir / (base.toStdString() + ".midi.jsonl")).string());

        QFile f(tomlPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QMessageBox::warning(this, tr("Save failed"), f.errorString());
            return;
        }
        const QString tomlStr = deriveKitToml();
        f.write(tomlStr.toUtf8());
        f.close();

        QFile rf(rawPath);
        if (rf.open(QIODevice::WriteOnly | QIODevice::Text)) {
            const QString capture_uuid =
                QUuid::createUuid().toString(QUuid::WithoutBraces);
            QByteArray host_seed("shadps4-kit-probe-v6:");
            host_seed += QSysInfo::machineUniqueId();
            const QString host_hash = QString::fromLatin1(
                QCryptographicHash::hash(host_seed, QCryptographicHash::Sha256)
                    .toHex().left(16));
            const char* dtStr =
                (m_deviceType == DeviceType::ProDrum) ? "pro_drum" : "drum";
            QString meta = QStringLiteral(
                "{\"type\":\"meta\",\"schema\":\"shadps4-midi-instrument/v1\","
                "\"version\":1,"
                "\"device_name\":\"%1\",\"port_id\":\"%2\","
                "\"source\":\"midi\",\"device_type\":\"%3\","
                "\"timestamp\":\"%4\","
                "\"capture_uuid\":\"%5\",\"host_hash\":\"%6\"}\n")
                .arg(QString(m_deviceName).replace('"', '\''))
                .arg(QString(m_midiPortId).replace('"', '\''))
                .arg(QString::fromLatin1(dtStr))
                .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate))
                .arg(capture_uuid)
                .arg(host_hash);
            rf.write(meta.toUtf8());
            rf.write("{\"step\":\"_idle_baseline\",\"events\":[]}\n");
            for (std::size_t i = 0; i < m_midi_results.size(); ++i) {
                const auto& buf = m_midi_results[i];
                if (!buf.captured) continue;
                QString line = QStringLiteral("{\"step\":\"%1\",\"events\":[")
                                   .arg(m_results[i].def.key);
                for (std::size_t j = 0; j < buf.events.size(); ++j) {
                    if (j) line += ',';
                    line += QStringLiteral(
                                "{\"on\":%1,\"note\":%2,\"vel\":%3,\"t_ms\":%4}")
                                .arg(buf.events[j].on ? "true" : "false")
                                .arg(buf.events[j].note)
                                .arg(buf.events[j].velocity)
                                .arg(buf.events[j].t_ms);
                }
                line += "]}\n";
                rf.write(line.toUtf8());
            }
            rf.close();
        }

        // Hot-reload: drop the kit into g_kits and close any open slot
        // that was bound to this vid:pid so the runtime PollLoop rebinds
        // against the just-written TOML on its next tick. Without this
        // the user has to close + reopen the game to pick up changes.
        Input::HidInstrument::LoadKitFile(tomlPath.toStdString());
        QMessageBox::information(this, tr("Saved"),
            tr("Saved into your shadPS4 user folder:\n\n"
               "  %1   (runtime kit definition)\n"
               "  %2   (raw MIDI event capture, for re-deriving later)\n\n"
               "Re-binding live — your kit's new note map is active immediately.")
                .arg(tomlPath).arg(rawPath));
        accept();
        return;
    }

    // Hash the device name into the filename — two Santroller devices with the same
    // VID:PID but different firmware types would otherwise overwrite each other.
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
    const QString tomlStr = deriveKitToml();
    f.write(tomlStr.toUtf8());
    f.close();

    bool seems_off = false;
    if (m_deviceType == DeviceType::Guitar || m_deviceType == DeviceType::GuitarSolo) {
        if (!tomlStr.contains(QStringLiteral("fret_byte")) &&
            !tomlStr.contains(QStringLiteral("fret_mask"))) {
            seems_off = true;
        }
        if (!tomlStr.contains(QStringLiteral("strum_down\""))) {
            seems_off = true;
        }
    } else {
        if (!tomlStr.contains(QStringLiteral("dud0_bit_remap"))) {
            seems_off = true;
        }
    }

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
        // capture_uuid: random per-capture id so two probes of the same
        // device on the same PC can still be told apart in shared logs.
        const QString capture_uuid =
            QUuid::createUuid().toString(QUuid::WithoutBraces);
        // host_hash: stable per-machine hash so multiple captures from the
        // same PC group together, but reversible-to-real-machine info
        // (hostname, MAC) is not exposed. SHA-256 of machineUniqueId with
        // a project-specific salt; first 16 hex chars is enough collision
        // resistance for the small community kit corpus.
        QByteArray host_seed("shadps4-kit-probe-v6:");
        host_seed += QSysInfo::machineUniqueId();
        const QString host_hash = QString::fromLatin1(
            QCryptographicHash::hash(host_seed, QCryptographicHash::Sha256)
                .toHex().left(16));
        QString meta = QStringLiteral(
            "{\"type\":\"meta\",\"version\":6,"
            "\"vendor_id\":\"0x%1\",\"product_id\":\"0x%2\","
            "\"device_name\":\"%3\",\"device_type\":\"%4\","
            "\"source\":\"%5\","
            "\"report_length\":%6,\"timestamp\":\"%7\","
            "\"capture_uuid\":\"%8\",\"host_hash\":\"%9\"}\n")
            .arg(m_vid, 4, 16, QChar('0'))
            .arg(m_pid, 4, 16, QChar('0'))
            .arg(QString(m_deviceName).replace('"', '\''))
            .arg(QString::fromLatin1(deviceTypeStr))
            .arg(QString::fromLatin1(sourceStr))
            .arg(m_reportLen)
            .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate))
            .arg(capture_uuid)
            .arg(host_hash);
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

    QString msg = tr("Saved into your shadPS4 user folder:\n\n"
                     "  %1   (runtime kit definition, auto-loaded at next launch)\n"
                     "  %2   (raw HID captures, for re-deriving the mapping later)")
                      .arg(tomlPath).arg(rawPath);

    if (seems_off) {
        msg += tr("\n\nWARNING: It seems something is off with the generated mapping! "
                  "If your device acts weird, please send the generated .toml and "
                  ".raw.jsonl files to the developer.");
    }

    QMessageBox::information(this, seems_off ? tr("Saved (with warnings)") : tr("Saved"), msg);
    accept();
}
