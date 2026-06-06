// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "player_assignment_dialog.h"

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QProgressBar>
#include <QPushButton>
#include <QSlider>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <chrono>
#include <filesystem>
#include <set>
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hidapi.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_joystick.h>

#include <cmath>

#include "common/config.h"
#include "common/path_util.h"
#include "device_picker_dialog.h"
#include "input/controller.h"
#include "input/hid_instrument.h"
#include "input/hid_kit_def.h"
#include "input/input_handler.h"
#include "input/midi_input.h"
#include "kit_probe_dialog.h"

namespace {

QString deviceDisplayLabel(const Config::PlayerDevice& dev) {
    using Kind = Config::PlayerDeviceKind;
    switch (dev.kind) {
    case Kind::Gamepad: {
        int n = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&n);
        QString name;
        u16 vid = 0, pid = 0;
        for (int i = 0; ids && i < n; ++i) {
            char buf[33];
            SDL_GUIDToString(SDL_GetJoystickGUIDForID(ids[i]), buf, sizeof(buf));
            if (std::string(buf) == dev.guid) {
                const char* nm = SDL_GetJoystickNameForID(ids[i]);
                if (!nm || !*nm)
                    nm = SDL_GetGamepadNameForID(ids[i]);
                if (nm && *nm)
                    name = QString::fromUtf8(nm);
                vid = SDL_GetJoystickVendorForID(ids[i]);
                pid = SDL_GetJoystickProductForID(ids[i]);
                break;
            }
        }
        if (ids)
            SDL_free(ids);
        // Tag XInput-source probed kits in the device list too — the
        // user might add a "Generic XInput Guitar" and want to see at a
        // glance that it's being driven via a probed kit rather than as
        // a plain DS4-shaped gamepad.
        QString probedSuffix;
        if (vid != 0 || pid != 0) {
            std::lock_guard<std::mutex> lk(Input::HidInstrument::g_kits_mu);
            for (const auto& kd : Input::HidInstrument::g_kits) {
                if (kd.source == "xinput" && kd.vid == vid && kd.pid == pid) {
                    probedSuffix = QObject::tr("  (Probed — XInput kit)");
                    break;
                }
            }
        }
        const QString short_guid = QString::fromStdString(dev.guid).left(8);
        if (!name.isEmpty())
            return QStringLiteral("Gamepad: %1 (%2…)").arg(name, short_guid) + probedSuffix;
        return QStringLiteral("Gamepad: %1… (not connected)").arg(short_guid) + probedSuffix;
    }
    case Kind::Kit:
        return QStringLiteral("Kit: 0x%1:0x%2")
            .arg(dev.vid, 4, 16, QChar('0'))
            .arg(dev.pid, 4, 16, QChar('0'));
    case Kind::Keyboard:
        return QStringLiteral("Keyboard");
    case Kind::Midi: {
        // dev.guid is the OS-specific port id (RtMidi index on ALSA, hex
        // UID on CoreMIDI). Resolve to a human-readable port name via
        // RtMidi enumeration if the device is currently visible.
        const auto ports = Input::MidiInput::EnumerateInputPorts();
        for (const auto& p : ports) {
            if (p.id == dev.guid)
                return QStringLiteral("MIDI: %1").arg(QString::fromStdString(p.name));
        }
        return QStringLiteral("MIDI: port %1 (not connected)")
            .arg(QString::fromStdString(dev.guid));
    }
    }
    return {};
}

QListWidgetItem* makeDeviceItem(const Config::PlayerDevice& dev) {
    auto* item = new QListWidgetItem(deviceDisplayLabel(dev));
    item->setData(Qt::UserRole, QString::fromStdString(Config::encodePlayerDevice(dev)));
    return item;
}

// Item label → enum int. Same list as the kSpecialPadClasses table in
// special_devices_dialog.cpp (mirrors OrbisPadDeviceClass in
// core/libraries/pad/pad.h) so power users keep all the same picks the
// old per-slot grid had.
struct ClassChoice {
    int value;
    const char* label;
};
constexpr std::array<ClassChoice, 10> kClassChoices = {{
    {0, QT_TR_NOOP("Standard")},
    {1, QT_TR_NOOP("Guitar")},
    {2, QT_TR_NOOP("Drum")},
    {3, QT_TR_NOOP("DJ Turntable")},
    {4, QT_TR_NOOP("Dancemat")},
    {5, QT_TR_NOOP("Navigation")},
    {6, QT_TR_NOOP("Steering Wheel")},
    {7, QT_TR_NOOP("Stick")},
    {8, QT_TR_NOOP("Fight Stick")},
    {9, QT_TR_NOOP("Gun")},
}};

// Per-tab live VU widget: grey background, fill colour reflects the
// gate state (green when level >= threshold = gate open, dark grey
// when below = gate closed/muted), plus a yellow vertical line at the
// gate threshold position. The dialog updates level / threshold every
// tick.
class MicVuBar : public QWidget {
public:
    explicit MicVuBar(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(14);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    void setLevel(float pct /*0..1*/) {
        pct = std::clamp(pct, 0.0f, 1.0f);
        if (std::abs(pct - m_level) > 1e-3f) {
            m_level = pct;
            update();
        }
    }
    void setThreshold(float pct /*0..1*/) {
        pct = std::clamp(pct, 0.0f, 1.0f);
        if (std::abs(pct - m_threshold) > 1e-3f) {
            m_threshold = pct;
            update();
        }
    }
    // When true the bar uses the gate-open colour (green) for the
    // active fill; when false (gate disabled or below threshold) it
    // falls back to grey so the user immediately sees their voice
    // isn't passing through.
    void setGateOpen(bool open) {
        if (m_gate_open != open) {
            m_gate_open = open;
            update();
        }
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        const QRect r = rect();
        p.fillRect(r, QColor(0x22, 0x22, 0x22));
        p.setPen(QPen(QColor(0x55, 0x55, 0x55), 1));
        p.drawRect(r.adjusted(0, 0, -1, -1));
        const int fillW = static_cast<int>(r.width() * m_level);
        if (fillW > 0) {
            const QColor fill = m_gate_open ? QColor(0x4c, 0xc2, 0x5a)
                                            : QColor(0x66, 0x66, 0x66);
            p.fillRect(QRect(0, 0, fillW, r.height()), fill);
        }
        const int threshX = static_cast<int>(r.width() * m_threshold);
        p.setPen(QPen(QColor(0xff, 0xc0, 0x40), 2));
        p.drawLine(threshX, 1, threshX, r.height() - 2);
    }
private:
    float m_level = 0.0f;
    float m_threshold = 0.0f;
    bool m_gate_open = false;
};

// Walk the slot's PlayerDevices + g_kits to find the matching kit's
// device_class. Mirrors HidInstrument::GetActiveKitDeviceClass(slot),
// but works in the launcher where PollLoop hasn't run yet — we look
// up by the slot's configured vid/pid (HID kits) or port_id (MIDI).
// Returns "" when no kit covers the slot's devices.
std::string DeriveKitClassFromSlotDevices(int slot) {
    std::lock_guard<std::mutex> lk(Input::HidInstrument::g_kits_mu);
    for (const auto& dev : Config::getPlayerSlotDevices(slot)) {
        if (dev.kind == Config::PlayerDeviceKind::Kit) {
            for (const auto& kd : Input::HidInstrument::g_kits) {
                if (kd.vid == dev.vid && kd.pid == dev.pid)
                    return kd.device_class;
            }
        } else if (dev.kind == Config::PlayerDeviceKind::Midi) {
            for (const auto& kd : Input::HidInstrument::g_kits) {
                if (kd.source == "midi" && kd.midi_port_id == dev.guid)
                    return kd.device_class;
            }
        }
    }
    return "";
}

int KitClassStringToInt(const std::string& s) {
    if (s == "guitar")       return 1;
    if (s == "drum")         return 2;
    if (s == "dj_turntable" || s == "turntable") return 3;
    if (s == "dance_mat"  || s == "dancemat")    return 4;
    return 0;
}

bool udevRulesInstalled() {
#ifdef __linux__
    std::error_code ec;
    return std::filesystem::exists("/etc/udev/rules.d/99-shadps4-instruments.rules", ec);
#else
    return true;
#endif
}

} // namespace

PlayerAssignmentDialog::PlayerAssignmentDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Player Assignment Overrides"));
    setModal(true);
    resize(720, 620);

    // Force-load every kit TOML in ~/.local/share/shadPS4/kits before
    // any UI code reads g_kits. The runtime normally triggers this
    // lazily on first scePadRead / libusb enumeration, but the launcher
    // never hits those paths — without this call, every previously
    // probed device would appear as "not yet probed" in the picker.
    Input::HidInstrument::EnsureKitsLoaded();

    auto* root = new QVBoxLayout(this);

    auto* help = new QLabel(tr("Configure each PS4 player slot. Each tab has the devices "
                               "pinned to that slot, the device class the game sees, the "
                               "legacy raw-HID pass-through toggle, and the mic input + gate. "
                               "The tab title flashes when input is detected on its slot, so "
                               "you can tell which controller is on which player at a glance."),
                            this);
    help->setWordWrap(true);
    help->setStyleSheet(QStringLiteral("color: #888;"));
    root->addWidget(help);

    m_tabs = new QTabWidget(this);
    root->addWidget(m_tabs, 1);

    for (int slot = 1; slot <= 4; ++slot) {
        buildSlotTab(slot);
    }

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                    Qt::Horizontal, this);
    root->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, this, &PlayerAssignmentDialog::onAccept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // Open every currently-attached SDL gamepad so we can poll their
    // button state from the dialog tick. The runtime FinalizeUpdate path
    // doesn't fire while the launcher is on top — we need our own pump.
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    int pad_count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&pad_count)) {
        for (int i = 0; i < pad_count; ++i) {
            if (SDL_Gamepad* gp = SDL_OpenGamepad(ids[i]))
                m_polled_pads[ids[i]] = gp;
        }
        SDL_free(ids);
    }
    // For each slot, open whatever MIDI port the slot's current device
    // list points at so the tab indicator + row glow react to incoming
    // MIDI events while no game window is up.
    for (int slot = 1; slot <= 4; ++slot) refreshPolledMidi(slot);
    // Keyboard event filter — dialog-level Tab/Enter/etc. presses bump
    // whichever slot has a Keyboard PlayerDevice configured.
    installEventFilter(this);

    // Drive the tab indicator + the VU meter at ~20 ms (50 Hz). Higher
    // refresh keeps the VU level visually in sync with what the user
    // hears in the room.
    auto* timer = new QTimer(this);
    timer->setInterval(20);
    connect(timer, &QTimer::timeout, this, [this]() {
        // 1. Sample SDL gamepad state so the tab indicator works without
        //    a running game window.
        SDL_UpdateGamepads();
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
        for (auto& [id, gp] : m_polled_pads) {
            if (!gp) continue;
            bool any = false;
            for (auto b : kButtons) {
                if (SDL_GetGamepadButton(gp, b)) { any = true; break; }
            }
            if (!any) continue;
            // The runtime FindBoundSlotForGamepad is the source of
            // truth for "which slot would this pad land on" — use it
            // here so the green tab/row indicator reflects what RB4
            // (and the rest of the runtime) actually sees. Falls back
            // to SDL_GetGamepadPlayerIndex when no binding matches,
            // which mirrors the runtime's pass-2 placement.
            char guid_buf[33];
            SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), guid_buf, sizeof(guid_buf));
            const char* path_c = SDL_GetJoystickPathForID(id);
            const std::string path = path_c ? path_c : std::string();
            int slot = Input::FindBoundSlotForGamepad(guid_buf, path);
            if (slot >= 1 && slot <= 4) {
                Input::NoteInputOnSlot(slot - 1);
            } else {
                slot = SDL_GetGamepadPlayerIndex(gp);
                if (slot >= 0 && slot < 4)
                    Input::NoteInputOnSlot(slot);
            }
        }
        // Drain MIDI events from each opened slot port. Any event
        // bumps the slot's input timestamp, lighting the tab + the
        // MIDI row regardless of whether a kit has been probed yet.
        for (int slot = 0; slot < 4; ++slot) {
            if (!m_polled_midi[slot]) continue;
            auto evs = Input::MidiInput::DrainEvents(m_polled_midi[slot]);
            if (!evs.empty()) {
                Input::NoteInputOnSlot(slot);
                m_last_midi_ns[slot] = std::chrono::steady_clock::now();
            }
        }

        // 2. Update tab text colour using a stylesheet so the change is
        //    actually visible. Plain "●" prefix is easy to miss.
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const u64 now_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
        constexpr u64 kFadeNs = 300'000'000ull; // 300 ms
        // Pre-compute "which gamepad GUIDs are pressed right now" so the
        // device-row sweep below can stop at the first GUID match.
        std::set<std::string> pressed_pad_guids;
        for (auto& [id, gp] : m_polled_pads) {
            if (!gp) continue;
            bool any = false;
            for (auto b : kButtons) {
                if (SDL_GetGamepadButton(gp, b)) { any = true; break; }
            }
            if (any) {
                char buf[33];
                SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), buf, sizeof(buf));
                pressed_pad_guids.insert(buf);
            }
        }
        const QBrush glowBrush(QColor(0x4c, 0xc2, 0x5a, 0x80));
        const QBrush emptyBrush;
        for (int slot = 0; slot < 4; ++slot) {
            const u64 last = Input::GetLastInputNs(slot);
            const bool active = last != 0 && (now_ns - last) < kFadeNs;
            m_tabs->tabBar()->setTabTextColor(
                slot, active ? QColor(0x4c, 0xc2, 0x5a) : QColor());

            // VU meter: drain the dialog-local preview stream, compute
            // RMS → dBFS, map onto 0..1. We use the dialog's own SDL
            // recording stream because Libraries::AudioIn::GetMicPeakDbfs
            // is silent until a game has actually opened sceAudioIn —
            // useless while the user is in settings.
            float lvl = 0.0f;
            if (auto* stream = m_slots[slot].previewStream) {
                const int avail = SDL_GetAudioStreamAvailable(stream);
                if (avail > 0) {
                    static thread_local std::vector<int16_t> buf;
                    const int to_read = std::min(avail, 4096);
                    buf.resize(to_read / sizeof(int16_t));
                    const int got = SDL_GetAudioStreamData(stream, buf.data(), to_read);
                    if (got > 0) {
                        const int count = got / static_cast<int>(sizeof(int16_t));
                        double sum_sq = 0.0;
                        for (int i = 0; i < count; ++i) {
                            const double v = static_cast<double>(buf[i]) / 32768.0;
                            sum_sq += v * v;
                        }
                        const double rms = count > 0 ? std::sqrt(sum_sq / count) : 0.0;
                        const double dbfs = rms > 1e-7 ? 20.0 * std::log10(rms) : -120.0;
                        lvl = static_cast<float>(
                            std::clamp((dbfs + 60.0) / 60.0, 0.0, 1.0));
                    }
                }
            }
            if (auto* vu = static_cast<MicVuBar*>(m_slots[slot].micVu)) {
                vu->setLevel(lvl);
                const int thr_db = m_slots[slot].micGateDb
                                       ? m_slots[slot].micGateDb->value()
                                       : -60;
                vu->setThreshold(std::clamp((thr_db + 60.0f) / 60.0f, 0.0f, 1.0f));
                // Gate-open / closed colour: fill stays green for the
                // hold window after the last above-threshold sample;
                // grey when below + hold-decayed or when gate disabled.
                const bool gate_enabled = m_slots[slot].micGateCb &&
                                          m_slots[slot].micGateCb->isChecked();
                if (!gate_enabled) {
                    vu->setGateOpen(true);
                } else {
                    const float thr_lvl =
                        std::clamp((thr_db + 60.0f) / 60.0f, 0.0f, 1.0f);
                    const auto now_tp = std::chrono::steady_clock::now();
                    if (lvl >= thr_lvl) {
                        m_slots[slot].lastAboveThreshold = now_tp;
                        m_slots[slot].gateOpen = true;
                    } else {
                        const int hold_ms = m_slots[slot].micGateHoldMs
                                                ? m_slots[slot].micGateHoldMs->value()
                                                : 300;
                        const auto since = std::chrono::duration_cast<
                            std::chrono::milliseconds>(
                            now_tp - m_slots[slot].lastAboveThreshold).count();
                        if (since > hold_ms) m_slots[slot].gateOpen = false;
                    }
                    vu->setGateOpen(m_slots[slot].gateOpen);
                }
            }

            // Per-row glow inside the slot's device list. Gamepad rows
            // light when the SDL pad with their GUID currently has any
            // standard button held; Keyboard rows light while any key
            // is held in the dialog; Kit / MIDI rows fall back to the
            // slot-level activity timestamp (we don't have per-device
            // tracking for those yet, but the user already sees the
            // tab glow when those bump).
            auto* list = m_slots[slot].list;
            if (!list) continue;
            for (int i = 0; i < list->count(); ++i) {
                auto* item = list->item(i);
                const QString enc = item->data(Qt::UserRole).toString();
                Config::PlayerDevice dev;
                if (!Config::decodePlayerDevice(enc.toStdString(), dev)) continue;
                bool glow = false;
                using Kind = Config::PlayerDeviceKind;
                switch (dev.kind) {
                case Kind::Gamepad:
                    glow = pressed_pad_guids.count(dev.guid) > 0;
                    break;
                case Kind::Keyboard:
                    glow = m_keyboard_held;
                    break;
                case Kind::Midi: {
                    // MIDI row uses its own per-slot timestamp so the
                    // row glow only fires when *this slot's* MIDI port
                    // produced an event — slot-level activity might be
                    // a gamepad pressed on the same slot, which would
                    // be confusing as a MIDI row glow.
                    const auto since = std::chrono::duration_cast<
                        std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - m_last_midi_ns[slot])
                                            .count();
                    glow = since < 300;
                    break;
                }
                case Kind::Kit:
                    // No per-device tracking — fall back to slot-level
                    // activity. Tab glow already shows this; this just
                    // makes the row reflect the same "something here
                    // fired" signal.
                    glow = active;
                    break;
                }
                item->setBackground(glow ? glowBrush : emptyBrush);
            }
        }
    });
    timer->start();
}

PlayerAssignmentDialog::~PlayerAssignmentDialog() {
    for (auto& [id, gp] : m_polled_pads) {
        if (gp) SDL_CloseGamepad(gp);
    }
    m_polled_pads.clear();
    for (auto& port : m_polled_midi) {
        if (port) Input::MidiInput::CloseInputPort(port);
        port = nullptr;
    }
    for (int slot = 1; slot <= 4; ++slot) {
        closeMicPreview(slot);
    }
}

bool PlayerAssignmentDialog::eventFilter(QObject* obj, QEvent* event) {
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
        m_keyboard_held = (event->type() == QEvent::KeyPress);
        // Find the slot whose PlayerDevices currently contain a Keyboard
        // entry and bump its input timestamp. We re-query Config rather
        // than caching because the user might add/remove the keyboard
        // device inside this same dialog session.
        for (int slot = 1; slot <= 4; ++slot) {
            for (const auto& dev : Config::getPlayerSlotDevices(slot)) {
                if (dev.kind == Config::PlayerDeviceKind::Keyboard) {
                    Input::NoteInputOnSlot(slot - 1);
                    break;
                }
            }
        }
    }
    return QDialog::eventFilter(obj, event);
}

void PlayerAssignmentDialog::buildSlotTab(int slot) {
    auto& w = m_slots[slot - 1];

    auto* tab = new QWidget(m_tabs);
    auto* root = new QVBoxLayout(tab);

    // --- Devices section -----------------------------------------------------
    auto* devicesBox = new QGroupBox(tr("Devices pinned to Player %1").arg(slot), tab);
    auto* devicesLayout = new QVBoxLayout(devicesBox);
    w.list = new QListWidget(devicesBox);
    w.list->setSelectionMode(QAbstractItemView::SingleSelection);
    devicesLayout->addWidget(w.list);

    auto* btnRow = new QHBoxLayout();
    auto* addGp = new QPushButton(tr("Add Gamepad…"), devicesBox);
    auto* addKit = new QPushButton(tr("Add Kit…"), devicesBox);
    auto* addKbd = new QPushButton(tr("Add Keyboard"), devicesBox);
    auto* addMidi = new QPushButton(tr("Add MIDI…"), devicesBox);
    auto* remove = new QPushButton(tr("Remove"), devicesBox);
    btnRow->addWidget(addGp);
    btnRow->addWidget(addKit);
    btnRow->addWidget(addKbd);
    btnRow->addWidget(addMidi);
    btnRow->addStretch();
    btnRow->addWidget(remove);
    devicesLayout->addLayout(btnRow);
    connect(addGp, &QPushButton::clicked, this, [this, slot]() { addGamepadDevice(slot); });
    connect(addKit, &QPushButton::clicked, this, [this, slot]() { addKitDevice(slot); });
    connect(addKbd, &QPushButton::clicked, this, [this, slot]() { addKeyboardDevice(slot); });
    connect(addMidi, &QPushButton::clicked, this, [this, slot]() { addMidiDevice(slot); });
    connect(remove, &QPushButton::clicked, this,
            [this, slot]() { removeSelectedDevice(slot); });
    root->addWidget(devicesBox);

    // --- Reports as + Legacy raw-HID -----------------------------------------
    auto* sysBox = new QGroupBox(tr("Game-visible device class"), tab);
    auto* sysLayout = new QVBoxLayout(sysBox);

    auto* classRow = new QHBoxLayout();
    auto* classLabel = new QLabel(tr("Reports as:"), sysBox);
    w.classCombo = new QComboBox(sysBox);
    for (const auto& c : kClassChoices) {
        w.classCombo->addItem(tr(c.label), c.value);
    }
    w.classCombo->setToolTip(tr("Game-visible device class for this slot. Games such as Rock "
                                "Band only enable their multi-instrument JOIN UI when a slot "
                                "reports a non-Standard class. When Legacy raw-HID is on AND a "
                                "kit is loaded, this picker is locked to \"Automatic\" — the "
                                "kit TOML drives the class instead."));
    classRow->addWidget(classLabel);
    classRow->addWidget(w.classCombo, 1);
    sysLayout->addLayout(classRow);

    // Legacy raw-HID pass-through is no longer a user-toggleable
    // checkbox — it auto-turns-on whenever a Kit / MIDI / XInput entry
    // sits in this slot's device list, since those paths only make
    // sense via the legacy path. The label below shows whichever state
    // we derived so the user still sees what's running.
    auto* legacyRow = new QHBoxLayout();
    w.legacyCb = new QCheckBox(QStringLiteral(""), sysBox);
    w.legacyCb->setVisible(false); // kept as backing storage only
    auto* legacyStatusLabel = new QLabel(sysBox);
    legacyStatusLabel->setObjectName(QStringLiteral("legacyStatus"));
    legacyStatusLabel->setToolTip(
        tr("Legacy raw-HID pass-through turns on automatically when the slot has a Kit, "
           "XInput, or MIDI device — those instruments need the raw HID / event stream "
           "bypassing SDL. Probe new devices via the button on the right."));
    w.probeBtn = new QPushButton(tr("Probe new kit…"), sysBox);
    w.udevWarn = new QLabel(sysBox);
    w.udevWarn->setStyleSheet(QStringLiteral("color: #d97706;"));
    legacyRow->addWidget(legacyStatusLabel, 1);
    legacyRow->addWidget(w.udevWarn);
    legacyRow->addWidget(w.probeBtn);
    sysLayout->addLayout(legacyRow);
    connect(w.probeBtn, &QPushButton::clicked, this, [this, slot]() { launchProbeWizard(slot); });
    root->addWidget(sysBox);

    // --- Mic ----------------------------------------------------------------
    auto* micBox = new QGroupBox(tr("Mic input (Player %1 vocals)").arg(slot), tab);
    auto* micLayout = new QVBoxLayout(micBox);
    auto* micRow = new QHBoxLayout();
    auto* micLabel = new QLabel(tr("Device:"), micBox);
    w.micCombo = new QComboBox(micBox);
    w.micCombo->addItem(tr("None"), QStringLiteral("None"));
    w.micCombo->addItem(tr("Default Device"), QStringLiteral("Default Device"));
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    int mic_count = 0;
    SDL_AudioDeviceID* mic_devices = SDL_GetAudioRecordingDevices(&mic_count);
    if (mic_devices) {
        for (int i = 0; i < mic_count; ++i) {
            const SDL_AudioDeviceID id = mic_devices[i];
            const char* nm = SDL_GetAudioDeviceName(id);
            if (nm) {
                const QString qname = QString::fromUtf8(nm);
                // Persist the device NAME as the item's data. SDL audio
                // device IDs are runtime handles that re-enumerate to
                // different values between sessions (and sometimes
                // between dialog opens), so a saved id wouldn't match
                // the next time the dialog loads — the combo would
                // silently revert to "None" and a subsequent Save
                // would persist that reset.
                w.micCombo->addItem(qname, qname);
            }
        }
        SDL_free(mic_devices);
    }
    w.micCombo->setToolTip(tr("Recording device routed to this player's sceAudioIn handle. "
                              "For RB4 vocal harmonies, set a different mic per slot."));
    micRow->addWidget(micLabel);
    micRow->addWidget(w.micCombo, 1);
    micLayout->addLayout(micRow);

    auto* gateRow = new QHBoxLayout();
    w.micGateCb = new QCheckBox(tr("Noise gate"), micBox);
    w.micGateCb->setToolTip(tr("Drops sub-threshold audio to silence so the game doesn't "
                                "score background noise as vocals."));
    w.micGateDb = new QSlider(Qt::Horizontal, micBox);
    w.micGateDb->setRange(-60, 0);
    w.micGateDb->setSingleStep(1);
    w.micGateDbLabel = new QLabel(micBox);
    auto updateGateLabel = [this, slot]() {
        m_slots[slot - 1].micGateDbLabel->setText(
            tr("Threshold: %1 dB").arg(m_slots[slot - 1].micGateDb->value()));
    };
    connect(w.micGateDb, &QSlider::valueChanged, this, updateGateLabel);
    connect(w.micGateCb, &QCheckBox::toggled, this, [this, slot](bool on) {
        m_slots[slot - 1].micGateDb->setEnabled(on);
        m_slots[slot - 1].micGateDbLabel->setEnabled(on);
        if (m_slots[slot - 1].micGateHoldMs) {
            m_slots[slot - 1].micGateHoldMs->setEnabled(on);
            m_slots[slot - 1].micGateHoldLabel->setEnabled(on);
        }
    });
    gateRow->addWidget(w.micGateCb);
    gateRow->addWidget(w.micGateDb, 1);
    gateRow->addWidget(w.micGateDbLabel);
    micLayout->addLayout(gateRow);

    // Hold time — how long the gate stays open after the voice drops
    // below threshold. Same unit (ms) the runtime uses via
    // Config::getMicGateHoldMs. 0..2000ms is the practical band: shorter
    // chops the tail of words, longer leaks background between phrases.
    auto* holdRow = new QHBoxLayout();
    auto* holdLabel = new QLabel(tr("Hold:"), micBox);
    w.micGateHoldMs = new QSlider(Qt::Horizontal, micBox);
    w.micGateHoldMs->setRange(0, 2000);
    w.micGateHoldMs->setSingleStep(50);
    w.micGateHoldLabel = new QLabel(micBox);
    auto updateHoldLabel = [this, slot]() {
        m_slots[slot - 1].micGateHoldLabel->setText(
            tr("%1 ms").arg(m_slots[slot - 1].micGateHoldMs->value()));
    };
    connect(w.micGateHoldMs, &QSlider::valueChanged, this, updateHoldLabel);
    holdRow->addWidget(holdLabel);
    holdRow->addWidget(w.micGateHoldMs, 1);
    holdRow->addWidget(w.micGateHoldLabel);
    micLayout->addLayout(holdRow);

    // VU meter — green level fill (live mic peak) + yellow gate
    // threshold line. Driven by Libraries::AudioIn::GetMicPeakDbfs in
    // the same tick that updates the tab indicator; threshold tracks
    // the slider above. Stays parked at silence until a game opens
    // sceAudioIn for this user_id.
    auto* vuRow = new QHBoxLayout();
    auto* vuLabel = new QLabel(tr("Level:"), micBox);
    auto* vu = new MicVuBar(micBox);
    vu->setToolTip(tr("Live mic input level (green) and gate threshold (yellow line). "
                      "Inactive until the game opens sceAudioIn for this player slot."));
    w.micVu = vu;
    vuRow->addWidget(vuLabel);
    vuRow->addWidget(vu, 1);
    micLayout->addLayout(vuRow);
    root->addWidget(micBox);

    root->addStretch();

    // Load current state for this slot.
    refreshSlotList(slot);

    const int currentClass =
        Config::getUseSpecialPad(slot) ? Config::getSpecialPadClass(slot) : 0;
    w.lastUserClass = currentClass;
    for (int i = 0; i < w.classCombo->count(); ++i) {
        if (w.classCombo->itemData(i).toInt() == currentClass) {
            w.classCombo->setCurrentIndex(i);
            break;
        }
    }
    w.legacyCb->setChecked(Config::getSpecialPadLegacyPassUSBRawHID(slot));

    const QString currentMic = QString::fromStdString(Config::getMicDevice(slot - 1));
    for (int i = 0; i < w.micCombo->count(); ++i) {
        if (w.micCombo->itemData(i).toString() == currentMic) {
            w.micCombo->setCurrentIndex(i);
            break;
        }
    }
    // Reopen the dialog-local recording stream whenever the user picks
    // a different mic so the VU previews against the new device.
    connect(w.micCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, slot](int) {
                closeMicPreview(slot);
                openMicPreview(slot);
            });
    openMicPreview(slot);
    const bool gateEnabled = Config::getMicGateEnabled(slot - 1);
    w.micGateCb->setChecked(gateEnabled);
    w.micGateDb->setValue(Config::getMicGateThresholdDb(slot - 1));
    w.micGateDb->setEnabled(gateEnabled);
    w.micGateDbLabel->setEnabled(gateEnabled);
    w.micGateHoldMs->setValue(Config::getMicGateHoldMs());
    w.micGateHoldMs->setEnabled(gateEnabled);
    w.micGateHoldLabel->setEnabled(gateEnabled);
    updateGateLabel();
    updateHoldLabel();

    refreshClassRow(slot);
    refreshLegacyVisuals(slot);

    m_tabs->addTab(tab, tr("Player %1").arg(slot));
}

void PlayerAssignmentDialog::refreshSlotList(int slot) {
    auto* list = m_slots[slot - 1].list;
    list->clear();
    for (const auto& dev : Config::getPlayerSlotDevices(slot)) {
        list->addItem(makeDeviceItem(dev));
    }
}

void PlayerAssignmentDialog::refreshClassRow(int slot) {
    auto& w = m_slots[slot - 1];
    const bool legacyOn = w.legacyCb->isChecked();
    // Prefer the runtime-bound kit's class (when the game is up); fall
    // back to deriving from the slot's configured devices + g_kits so
    // the launcher displays Automatic correctly even without a running
    // game.
    std::string kitClass = Input::HidInstrument::GetActiveKitDeviceClass(slot);
    if (kitClass.empty())
        kitClass = DeriveKitClassFromSlotDevices(slot);
    const bool automatic = legacyOn && !kitClass.empty();

    // Insert / remove a sentinel "Automatic" entry at index 0 so the
    // combo's currentText reflects what's actually driving the class.
    const bool hasSentinel =
        w.classCombo->count() > 0 && w.classCombo->itemData(0).toInt() == -1;
    if (automatic) {
        if (!hasSentinel) {
            const QString label =
                tr("Automatic — %1 (from kit TOML)").arg(QString::fromStdString(kitClass));
            w.classCombo->insertItem(0, label, -1);
        } else {
            const QString label =
                tr("Automatic — %1 (from kit TOML)").arg(QString::fromStdString(kitClass));
            w.classCombo->setItemText(0, label);
        }
        // Remember the user's pre-Automatic choice before we hijack
        // the selection, so toggling legacy off can restore it.
        if (w.classCombo->currentData().toInt() != -1)
            w.lastUserClass = w.classCombo->currentData().toInt();
        w.classCombo->setCurrentIndex(0);
        w.classCombo->setEnabled(false);
    } else {
        if (hasSentinel)
            w.classCombo->removeItem(0);
        w.classCombo->setEnabled(true);
        for (int i = 0; i < w.classCombo->count(); ++i) {
            if (w.classCombo->itemData(i).toInt() == w.lastUserClass) {
                w.classCombo->setCurrentIndex(i);
                break;
            }
        }
    }
}

void PlayerAssignmentDialog::refreshLegacyVisuals(int slot) {
    auto& w = m_slots[slot - 1];
    // Legacy raw-HID pass-through should auto-engage whenever the slot
    // has at least one device that can't go through the standard SDL
    // gamepad path — i.e. a Kit (raw HID / XInput) or MIDI entry. We
    // read from the dialog's live list (m_slots[].list) rather than
    // Config because the user's just-made edits aren't persisted yet.
    bool legacyOn = false;
    for (int i = 0; i < w.list->count(); ++i) {
        const QString enc = w.list->item(i)->data(Qt::UserRole).toString();
        Config::PlayerDevice dev;
        if (!Config::decodePlayerDevice(enc.toStdString(), dev)) continue;
        if (dev.kind == Config::PlayerDeviceKind::Kit ||
            dev.kind == Config::PlayerDeviceKind::Midi) {
            legacyOn = true;
            break;
        }
    }
    w.legacyCb->setChecked(legacyOn);
    auto* statusLabel = w.legacyCb->parentWidget()
        ? w.legacyCb->parentWidget()->findChild<QLabel*>(QStringLiteral("legacyStatus"))
        : nullptr;
    if (statusLabel) {
        if (legacyOn) {
            statusLabel->setText(tr("Legacy raw-HID pass-through: <b>on</b> "
                                    "(auto-enabled by Kit / MIDI device)"));
            statusLabel->setStyleSheet(QStringLiteral("color: #4cc25a;"));
        } else {
            statusLabel->setText(tr("Legacy raw-HID pass-through: off "
                                    "(no Kit / MIDI device pinned to this slot)"));
            statusLabel->setStyleSheet(QStringLiteral("color: #888;"));
        }
    }
    const bool udevOk = udevRulesInstalled();
    w.probeBtn->setVisible(true); // Probe wizard always reachable
    if (legacyOn && !udevOk) {
        w.udevWarn->setVisible(true);
        w.udevWarn->setText(
            tr("⚠ install udev rules first (Configure Special Devices)"));
    } else {
        w.udevWarn->setVisible(false);
        w.udevWarn->clear();
    }
}

void PlayerAssignmentDialog::addGamepadDevice(int slot) {
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    if (!ids || n == 0) {
        if (ids)
            SDL_free(ids);
        QMessageBox::information(this, tr("No gamepads detected"),
                                 tr("Plug in a controller and try again."));
        return;
    }
    DevicePickerDialog dlg(tr("Add Gamepad"),
                           tr("Choose a gamepad to bind to Player %1. Tap any button on the "
                              "controller to identify it — its row will glow green. Identical "
                              "controllers are told apart by their USB port (device path) so "
                              "the binding stays tied to the port until you move the cable.")
                               .arg(slot),
                           this);
    // Snapshot the kit library so we can tag any gamepad whose vid:pid
    // matches an XInput-source kit TOML. Helps the user tell their plain
    // Xbox 360 controller apart from a Xbox 360 guitar (same SDL name,
    // very different role).
    std::set<std::pair<u16, u16>> probed_xinput_kits;
    {
        std::lock_guard<std::mutex> lk(Input::HidInstrument::g_kits_mu);
        for (const auto& kd : Input::HidInstrument::g_kits) {
            if (kd.source == "xinput")
                probed_xinput_kits.insert({kd.vid, kd.pid});
        }
    }
    for (int i = 0; i < n; ++i) {
        char buf[33];
        SDL_GUIDToString(SDL_GetJoystickGUIDForID(ids[i]), buf, sizeof(buf));
        const char* nm = SDL_GetJoystickNameForID(ids[i]);
        if (!nm || !*nm)
            nm = SDL_GetGamepadNameForID(ids[i]);
        const QString name = nm ? QString::fromUtf8(nm) : QStringLiteral("?");
        const QString guid = QString::fromLatin1(buf);
        const char* path = SDL_GetJoystickPathForID(ids[i]);
        Config::PlayerDevice dev;
        dev.kind = Config::PlayerDeviceKind::Gamepad;
        dev.guid = guid.toStdString();
        if (path)
            dev.path = path;
        const QString enc = QString::fromStdString(Config::encodePlayerDevice(dev));
        QString label = QStringLiteral("%1 — %2…").arg(name, guid.left(12));
        if (path && *path) {
            QString p = QString::fromUtf8(path);
            if (p.size() > 28)
                p = QStringLiteral("…%1").arg(p.right(28));
            label += QStringLiteral("  [%1]").arg(p);
        }
        const u16 vid = SDL_GetJoystickVendorForID(ids[i]);
        const u16 pid = SDL_GetJoystickProductForID(ids[i]);
        if (probed_xinput_kits.count({vid, pid}))
            label += tr("  (Probed — XInput kit)");
        dlg.addRow(label, enc, true, QString(), ids[i]);
    }
    SDL_free(ids);
    if (dlg.exec() != QDialog::Accepted)
        return;
    Config::PlayerDevice dev;
    if (!Config::decodePlayerDevice(dlg.chosenEncoded().toStdString(), dev))
        return;
    m_slots[slot - 1].list->addItem(makeDeviceItem(dev));
    refreshLegacyVisuals(slot);
    refreshClassRow(slot);
}

// Custom two-section picker for kit / MIDI devices. Top section lists
// probed devices (a kit TOML exists, the user can pick them); bottom
// section lists detected-but-unprobed devices each with an inline
// "Probe…" button that launches KitProbeDialog. After a probe finishes
// we rescan g_kits and re-render so the just-probed device jumps from
// the unprobed list to the probed list without closing the picker.
namespace {

struct PickRow {
    QString label;     // shown to user
    QString encoded;   // Config::encodePlayerDevice() output
    bool probed;
};

QDialog* makeTwoSectionPicker(QWidget* parent, const QString& title, const QString& help,
                              const std::vector<PickRow>& rows,
                              std::function<void()> on_probe_clicked,
                              QString* out_encoded) {
    auto* dlg = new QDialog(parent);
    dlg->setWindowTitle(title);
    dlg->setModal(true);
    dlg->resize(560, 440);
    auto* root = new QVBoxLayout(dlg);
    if (!help.isEmpty()) {
        auto* label = new QLabel(help, dlg);
        label->setWordWrap(true);
        label->setStyleSheet(QStringLiteral("color: #888;"));
        root->addWidget(label);
    }

    auto* probedBox = new QGroupBox(QObject::tr("Probed (ready to use)"), dlg);
    auto* probedLayout = new QVBoxLayout(probedBox);
    auto* probedList = new QListWidget(probedBox);
    probedList->setSelectionMode(QAbstractItemView::SingleSelection);
    probedLayout->addWidget(probedList);
    root->addWidget(probedBox, 1);

    auto* unprobedBox = new QGroupBox(QObject::tr("Detected but not yet probed"), dlg);
    auto* unprobedLayout = new QVBoxLayout(unprobedBox);
    auto* unprobedList = new QListWidget(unprobedBox);
    unprobedList->setSelectionMode(QAbstractItemView::NoSelection);
    unprobedLayout->addWidget(unprobedList);
    root->addWidget(unprobedBox, 1);

    int probedCount = 0;
    int unprobedCount = 0;
    for (const auto& r : rows) {
        if (r.probed) {
            auto* item = new QListWidgetItem(r.label, probedList);
            item->setData(Qt::UserRole, r.encoded);
            ++probedCount;
        } else {
            // Inline "Probe…" row: a horizontal widget with a label + button.
            auto* item = new QListWidgetItem(probedList /*sentinel*/);
            // ^ unused — we use setItemWidget on a fresh row instead so the
            // row gets a real button. The item above prevents an empty list.
            delete item;
            auto* rowItem = new QListWidgetItem(unprobedList);
            auto* rowWidget = new QWidget();
            auto* rowLayout = new QHBoxLayout(rowWidget);
            rowLayout->setContentsMargins(4, 2, 4, 2);
            rowLayout->addWidget(new QLabel(r.label, rowWidget), 1);
            auto* probeBtn = new QPushButton(QObject::tr("Probe…"), rowWidget);
            rowLayout->addWidget(probeBtn);
            QObject::connect(probeBtn, &QPushButton::clicked, rowWidget,
                             [on_probe_clicked]() { on_probe_clicked(); });
            rowItem->setSizeHint(rowWidget->sizeHint());
            unprobedList->setItemWidget(rowItem, rowWidget);
            ++unprobedCount;
        }
    }
    if (probedCount == 0) {
        auto* item = new QListWidgetItem(QObject::tr("No probed devices yet."), probedList);
        item->setForeground(QBrush(QColor(0x88, 0x88, 0x88)));
        item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
    }
    if (unprobedCount == 0) {
        auto* item = new QListWidgetItem(QObject::tr("Nothing new detected."), unprobedList);
        item->setForeground(QBrush(QColor(0x88, 0x88, 0x88)));
        item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
    }

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                    Qt::Horizontal, dlg);
    auto* okBtn = bb->button(QDialogButtonBox::Ok);
    okBtn->setEnabled(false);
    QObject::connect(probedList, &QListWidget::itemSelectionChanged, dlg,
                     [probedList, okBtn]() {
                         okBtn->setEnabled(!probedList->selectedItems().isEmpty() &&
                                           probedList->selectedItems().front()->flags() &
                                               Qt::ItemIsSelectable);
                     });
    QObject::connect(bb, &QDialogButtonBox::accepted, dlg, [dlg, probedList, out_encoded]() {
        if (probedList->selectedItems().isEmpty()) return;
        auto* item = probedList->selectedItems().front();
        if (out_encoded) *out_encoded = item->data(Qt::UserRole).toString();
        dlg->accept();
    });
    QObject::connect(bb, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
    root->addWidget(bb);
    return dlg;
}

} // namespace

void PlayerAssignmentDialog::addKitDevice(int slot) {
    // Helper: enumerate every detected HID VID:PID and mark it
    // probed/unprobed against the current g_kits snapshot. Returns the
    // PickRow vector ready for the picker, and lets the inline Probe
    // button re-call the same function after a probe completes.
    auto buildRows = []() {
        std::set<std::pair<u16, u16>> loaded;
        {
            std::lock_guard<std::mutex> lk(Input::HidInstrument::g_kits_mu);
            for (const auto& kd : Input::HidInstrument::g_kits) {
                loaded.insert({kd.vid, kd.pid});
            }
        }
        SDL_hid_init();
        std::set<std::pair<u16, u16>> seen;
        std::vector<PickRow> rows;
        if (auto* head = SDL_hid_enumerate(0, 0)) {
            for (auto* d = head; d; d = d->next) {
                const std::pair<u16, u16> key{d->vendor_id, d->product_id};
                if (seen.count(key)) continue;
                seen.insert(key);
                const std::string mfr = d->manufacturer_string
                    ? QString::fromWCharArray(d->manufacturer_string).trimmed().toStdString()
                    : "";
                const std::string prod = d->product_string
                    ? QString::fromWCharArray(d->product_string).trimmed().toStdString()
                    : "";
                char buf[128];
                std::snprintf(buf, sizeof(buf), "%04x:%04x  %s %s", d->vendor_id,
                              d->product_id, mfr.empty() ? "?" : mfr.c_str(), prod.c_str());
                PickRow r;
                r.label = QString::fromStdString(buf);
                Config::PlayerDevice dev;
                dev.kind = Config::PlayerDeviceKind::Kit;
                dev.vid = d->vendor_id;
                dev.pid = d->product_id;
                r.encoded = QString::fromStdString(Config::encodePlayerDevice(dev));
                r.probed = loaded.count(key) > 0;
                rows.push_back(std::move(r));
            }
            SDL_hid_free_enumeration(head);
        }
        return rows;
    };

    std::vector<PickRow> rows = buildRows();
    if (rows.empty()) {
        QMessageBox::information(this, tr("No HID devices detected"),
                                 tr("Plug in your instrument, then open this dialog again."));
        return;
    }
    QString chosen;
    QDialog* dlgPtr = nullptr;
    // Probing through the inline button rebuilds the row list and
    // re-opens the picker so the just-probed device shows up in the
    // probed section without the user having to close + re-open the
    // whole Player Assignment menu. Custom done() code distinguishes
    // "probe-done, please re-loop" from "user cancelled".
    constexpr int kProbeDone = 100;
    auto on_probe = [this, &dlgPtr]() {
        KitProbeDialog wiz(this);
        wiz.exec();
        if (dlgPtr) dlgPtr->done(kProbeDone);
    };
    while (true) {
        dlgPtr = makeTwoSectionPicker(this, tr("Add Kit"),
                                      tr("Pick a probed kit to bind to Player %1, or click "
                                         "Probe… next to a new device to register it.")
                                          .arg(slot),
                                      rows, on_probe, &chosen);
        const int result = dlgPtr->exec();
        delete dlgPtr;
        dlgPtr = nullptr;
        if (result == QDialog::Accepted && !chosen.isEmpty()) break;
        if (result == kProbeDone) {
            rows = buildRows();
            continue;
        }
        return; // Rejected = user cancelled
    }

    Config::PlayerDevice dev;
    if (!Config::decodePlayerDevice(chosen.toStdString(), dev))
        return;
    m_slots[slot - 1].list->addItem(makeDeviceItem(dev));
    refreshLegacyVisuals(slot);
    refreshClassRow(slot);
}

void PlayerAssignmentDialog::addKeyboardDevice(int slot) {
    auto* list = m_slots[slot - 1].list;
    for (int i = 0; i < list->count(); ++i) {
        if (list->item(i)->data(Qt::UserRole).toString() == QLatin1String("keyboard")) {
            QMessageBox::information(this, tr("Keyboard already added"),
                                     tr("Player %1 already has the keyboard.").arg(slot));
            return;
        }
    }
    Config::PlayerDevice dev;
    dev.kind = Config::PlayerDeviceKind::Keyboard;
    list->addItem(makeDeviceItem(dev));
    refreshLegacyVisuals(slot);
    refreshClassRow(slot);
}

void PlayerAssignmentDialog::addMidiDevice(int slot) {
    // Probed = the port's id appears in a loaded kit's midi_port_id.
    auto buildRows = []() {
        std::set<std::string> probed_port_ids;
        {
            std::lock_guard<std::mutex> lk(Input::HidInstrument::g_kits_mu);
            for (const auto& kd : Input::HidInstrument::g_kits) {
                if (!kd.midi_port_id.empty())
                    probed_port_ids.insert(kd.midi_port_id);
            }
        }
        std::vector<PickRow> rows;
        for (const auto& p : Input::MidiInput::EnumerateInputPorts()) {
            PickRow r;
            r.label = QStringLiteral("%1  [port %2]")
                          .arg(QString::fromStdString(p.name),
                               QString::fromStdString(p.id));
            Config::PlayerDevice dev;
            dev.kind = Config::PlayerDeviceKind::Midi;
            dev.guid = p.id;
            r.encoded = QString::fromStdString(Config::encodePlayerDevice(dev));
            r.probed = probed_port_ids.count(p.id) > 0;
            rows.push_back(std::move(r));
        }
        return rows;
    };

    std::vector<PickRow> rows = buildRows();
    if (rows.empty()) {
        QMessageBox::information(this, tr("No MIDI ports detected"),
                                 tr("No MIDI input ports were found on this host. Plug in your "
                                    "module (or a USB-MIDI adapter), open this dialog again."));
        return;
    }
    QString chosen;
    QDialog* dlgPtr = nullptr;
    constexpr int kProbeDone = 100;
    auto on_probe = [this, &dlgPtr]() {
        KitProbeDialog wiz(this);
        wiz.exec();
        if (dlgPtr) dlgPtr->done(kProbeDone);
    };
    while (true) {
        dlgPtr = makeTwoSectionPicker(
            this, tr("Add MIDI"),
            tr("Pick a probed MIDI port to bind to Player %1, or click Probe… "
               "next to an unconfigured port to map its notes through the kit "
               "wizard.")
                .arg(slot),
            rows, on_probe, &chosen);
        const int result = dlgPtr->exec();
        delete dlgPtr;
        dlgPtr = nullptr;
        if (result == QDialog::Accepted && !chosen.isEmpty()) break;
        if (result == kProbeDone) {
            rows = buildRows();
            continue;
        }
        return;
    }

    Config::PlayerDevice dev;
    if (!Config::decodePlayerDevice(chosen.toStdString(), dev))
        return;
    m_slots[slot - 1].list->addItem(makeDeviceItem(dev));
    refreshLegacyVisuals(slot);
    refreshClassRow(slot);
    refreshPolledMidi(slot);
}

void PlayerAssignmentDialog::removeSelectedDevice(int slot) {
    auto* list = m_slots[slot - 1].list;
    for (auto* it : list->selectedItems())
        delete list->takeItem(list->row(it));
    refreshLegacyVisuals(slot);
    refreshClassRow(slot);
    refreshPolledMidi(slot);
}

void PlayerAssignmentDialog::refreshPolledMidi(int slot) {
    void*& port = m_polled_midi[slot - 1];
    if (port) {
        Input::MidiInput::CloseInputPort(port);
        port = nullptr;
    }
    // Prefer the live list (m_slots[].list) when it's been built;
    // otherwise fall back to Config (used at constructor time before
    // tabs exist).
    std::string port_id;
    auto* list = m_slots[slot - 1].list;
    if (list) {
        for (int i = 0; i < list->count(); ++i) {
            const QString enc = list->item(i)->data(Qt::UserRole).toString();
            Config::PlayerDevice dev;
            if (!Config::decodePlayerDevice(enc.toStdString(), dev)) continue;
            if (dev.kind == Config::PlayerDeviceKind::Midi) {
                port_id = dev.guid;
                break;
            }
        }
    } else {
        for (const auto& dev : Config::getPlayerSlotDevices(slot)) {
            if (dev.kind == Config::PlayerDeviceKind::Midi) {
                port_id = dev.guid;
                break;
            }
        }
    }
    if (port_id.empty()) return;
    port = Input::MidiInput::OpenInputPort(port_id);
    if (port) {
        LOG_INFO(Input, "PlayerAssignment: opened MIDI port '{}' for slot {}",
                 port_id, slot);
    } else {
        LOG_WARNING(Input, "PlayerAssignment: failed to open MIDI port '{}' for slot {}",
                    port_id, slot);
    }
}

void PlayerAssignmentDialog::openMicPreview(int slot) {
    auto& w = m_slots[slot - 1];
    if (!w.micCombo) return;
    if (w.previewStream) closeMicPreview(slot);
    const QString dev_data = w.micCombo->currentData().toString();
    if (dev_data == "None" || dev_data.isEmpty()) return;
    SDL_AudioDeviceID dev_id = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
    if (dev_data != "Default Device") {
        bool ok = false;
        const uint dev = dev_data.toUInt(&ok);
        if (ok) dev_id = static_cast<SDL_AudioDeviceID>(dev);
    }
    SDL_AudioSpec spec;
    SDL_zero(spec);
    spec.format = SDL_AUDIO_S16;
    spec.channels = 1;
    spec.freq = 44100;
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    // Hint a short capture window so the VU reflects what just landed
    // in the mic, not what landed 100+ ms ago. SDL reads this hint at
    // device-open time, so set it immediately before the open call.
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "256");
    w.previewStream = SDL_OpenAudioDeviceStream(dev_id, &spec, nullptr, nullptr);
    if (w.previewStream)
        SDL_ResumeAudioStreamDevice(w.previewStream);
}

void PlayerAssignmentDialog::closeMicPreview(int slot) {
    auto& w = m_slots[slot - 1];
    if (w.previewStream) {
        SDL_DestroyAudioStream(w.previewStream);
        w.previewStream = nullptr;
    }
}

void PlayerAssignmentDialog::launchProbeWizard(int slot) {
    KitProbeDialog wiz(this);
    wiz.exec();
    // Picked-up state for the slot might have changed (a kit just landed
    // in g_kits). Refresh the class row so the "Automatic" mode lights
    // up immediately without forcing the user to flip the legacy toggle.
    refreshClassRow(slot);
}

void PlayerAssignmentDialog::onAccept() {
    for (int slot = 1; slot <= 4; ++slot) {
        auto& w = m_slots[slot - 1];

        std::vector<Config::PlayerDevice> devs;
        devs.reserve(w.list->count());
        for (int i = 0; i < w.list->count(); ++i) {
            const QString enc = w.list->item(i)->data(Qt::UserRole).toString();
            Config::PlayerDevice dev;
            if (Config::decodePlayerDevice(enc.toStdString(), dev))
                devs.push_back(std::move(dev));
        }
        Config::setPlayerSlotDevices(slot, devs);

        // Class: prefer the combo's live selection. When the combo is
        // showing the "Automatic" sentinel (-1) we persist the kit's
        // own device_class so the game's scePadGetControllerInformation
        // returns the right class even before the runtime kit binding
        // takes effect (and as a fallback if the binding ever fails).
        int cls = w.classCombo->currentData().toInt();
        if (cls == -1) {
            const std::string kit_cls = DeriveKitClassFromSlotDevices(slot);
            cls = KitClassStringToInt(kit_cls);
            if (cls == 0) cls = w.lastUserClass; // probed but unrecognised class
        }
        Config::setUseSpecialPad(slot, cls != 0);
        Config::setSpecialPadClass(slot, cls);

        Config::setSpecialPadLegacyPassUSBRawHID(slot, w.legacyCb->isChecked());

        Config::setMicDevice(slot - 1, w.micCombo->currentData().toString().toStdString());
        Config::setMicGateEnabled(slot - 1, w.micGateCb->isChecked());
        Config::setMicGateThresholdDb(slot - 1, w.micGateDb->value());
        // Hold time is global, not per-slot — only write once from
        // whichever slot the user happens to have configured last.
        if (slot == 4)
            Config::setMicGateHoldMs(w.micGateHoldMs->value());
    }
    // Persist to disk — without this the per-slot mic device, gate
    // settings, and legacy / class state vanish on the next launch
    // because the global Config::save only fires from a handful of
    // top-level dialogs.
    Config::save(Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "config.toml");
    // Apply changes live: relocate connected gamepads whose GUID is now
    // bound to a different slot, then re-parse keybindings so keyboard
    // routing picks up any new Keyboard-slot mapping.
    Input::GameControllers::ApplyAssignmentChanges();
    Input::ParseInputConfig(Config::GetUseUnifiedInputConfig() ? std::string("default")
                                                               : std::string());
    accept();
}
