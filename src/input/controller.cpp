// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <unordered_set>
#include <SDL3/SDL.h>
#include <common/singleton.h>
#include "common/config.h"
#include "common/logging/log.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/pad/pad.h"
#include "core/libraries/system/userservice.h"
#include "input/controller.h"

static std::string SelectedGamepad = "";

namespace Input {

GameController::GameController() {
    m_states_num = 0;
    m_last_state = State();
}

void GameController::ReadState(State* state, bool* isConnected, int* connectedCount) {
    std::scoped_lock lock{m_mutex};

    *isConnected = m_connected;
    *connectedCount = m_connected_count;
    *state = GetLastState();
}

int GameController::ReadStates(State* states, int states_num, bool* isConnected,
                               int* connectedCount) {
    std::scoped_lock lock{m_mutex};

    *isConnected = m_connected;
    *connectedCount = m_connected_count;

    int ret_num = 0;

    if (m_connected) {
        if (m_states_num == 0) {
            ret_num = 1;
            states[0] = m_last_state;
        } else {
            for (uint32_t i = 0; i < m_states_num; i++) {
                if (ret_num >= states_num) {
                    break;
                }
                auto index = (m_first_state + i) % MAX_STATES;
                if (!m_private[index].obtained) {
                    m_private[index].obtained = true;

                    states[ret_num++] = m_states[index];
                }
            }
        }
    }

    return ret_num;
}

State GameController::GetLastState() const {
    if (m_states_num == 0) {
        return m_last_state;
    }
    const u32 last = (m_first_state + m_states_num - 1) % MAX_STATES;
    return m_states[last];
}

void GameController::AddState(const State& state) {
    if (m_states_num >= MAX_STATES) {
        m_states_num = MAX_STATES - 1;
        m_first_state = (m_first_state + 1) % MAX_STATES;
    }

    const u32 index = (m_first_state + m_states_num) % MAX_STATES;
    m_states[index] = state;
    m_last_state = state;
    m_private[index].obtained = false;
    m_states_num++;
}

void GameController::CheckButton(int id, Libraries::Pad::OrbisPadButtonDataOffset button,
                                 bool is_pressed) {
    std::scoped_lock lock{m_mutex};
    auto state = GetLastState();
    state.time = Libraries::Kernel::sceKernelGetProcessTime();
    if (is_pressed) {
        state.buttonsState |= button;
    } else {
        state.buttonsState &= ~button;
    }

    AddState(state);
}

void GameController::Axis(int id, Input::Axis axis, int value) {
    using Libraries::Pad::OrbisPadButtonDataOffset;

    std::scoped_lock lock{m_mutex};
    auto state = GetLastState();

    state.time = Libraries::Kernel::sceKernelGetProcessTime();
    int axis_id = static_cast<int>(axis);
    state.axes[axis_id] = value;

    if (axis == Input::Axis::TriggerLeft) {
        if (value > 0) {
            state.buttonsState |= OrbisPadButtonDataOffset::L2;
        } else {
            state.buttonsState &= ~OrbisPadButtonDataOffset::L2;
        }
    }

    if (axis == Input::Axis::TriggerRight) {
        if (value > 0) {
            state.buttonsState |= OrbisPadButtonDataOffset::R2;
        } else {
            state.buttonsState &= ~OrbisPadButtonDataOffset::R2;
        }
    }

    AddState(state);
}

void GameController::Gyro(int id, const float gyro[3]) {
    std::scoped_lock lock{m_mutex};
    auto state = GetLastState();
    state.time = Libraries::Kernel::sceKernelGetProcessTime();

    // Update the angular velocity (gyro data)
    state.angularVelocity.x = gyro[0]; // X-axis
    state.angularVelocity.y = gyro[1]; // Y-axis
    state.angularVelocity.z = gyro[2]; // Z-axis

    AddState(state);
}
void GameController::Acceleration(int id, const float acceleration[3]) {
    std::scoped_lock lock{m_mutex};
    auto state = GetLastState();
    state.time = Libraries::Kernel::sceKernelGetProcessTime();

    // Update the acceleration values
    state.acceleration.x = acceleration[0]; // X-axis
    state.acceleration.y = acceleration[1]; // Y-axis
    state.acceleration.z = acceleration[2]; // Z-axis

    AddState(state);
}

// Stolen from
// https://github.com/xioTechnologies/Open-Source-AHRS-With-x-IMU/blob/master/x-IMU%20IMU%20and%20AHRS%20Algorithms/x-IMU%20IMU%20and%20AHRS%20Algorithms/AHRS/MahonyAHRS.cs
float eInt[3] = {0.0f, 0.0f, 0.0f}; // Integral error terms
const float Kp = 50.0f;             // Proportional gain
const float Ki = 1.0f;              // Integral gain
Libraries::Pad::OrbisFQuaternion o = {1, 0, 0, 0};
void GameController::CalculateOrientation(Libraries::Pad::OrbisFVector3& acceleration,
                                          Libraries::Pad::OrbisFVector3& angularVelocity,
                                          float deltaTime,
                                          Libraries::Pad::OrbisFQuaternion& orientation) {
    float ax = acceleration.x, ay = acceleration.y, az = acceleration.z;
    float gx = angularVelocity.x, gy = angularVelocity.y, gz = angularVelocity.z;

    float q1 = o.w, q2 = o.x, q3 = o.y, q4 = o.z;

    // Normalize accelerometer measurement
    float norm = std::sqrt(ax * ax + ay * ay + az * az);
    if (norm == 0.0f || deltaTime == 0.0f)
        return; // Handle NaN
    norm = 1.0f / norm;
    ax *= norm;
    ay *= norm;
    az *= norm;

    // Estimated direction of gravity
    float vx = 2.0f * (q2 * q4 - q1 * q3);
    float vy = 2.0f * (q1 * q2 + q3 * q4);
    float vz = q1 * q1 - q2 * q2 - q3 * q3 + q4 * q4;

    // Error is cross product between estimated direction and measured direction of gravity
    float ex = (ay * vz - az * vy);
    float ey = (az * vx - ax * vz);
    float ez = (ax * vy - ay * vx);
    if (Ki > 0.0f) {
        eInt[0] += ex * deltaTime; // Accumulate integral error
        eInt[1] += ey * deltaTime;
        eInt[2] += ez * deltaTime;
    } else {
        eInt[0] = eInt[1] = eInt[2] = 0.0f; // Prevent integral wind-up
    }

    // Apply feedback terms
    gx += Kp * ex + Ki * eInt[0];
    gy += Kp * ey + Ki * eInt[1];
    gz += Kp * ez + Ki * eInt[2];

    //// Integrate rate of change of quaternion
    q1 += (-q2 * gx - q3 * gy - q4 * gz) * (0.5f * deltaTime);
    q2 += (q1 * gx + q3 * gz - q4 * gy) * (0.5f * deltaTime);
    q3 += (q1 * gy - q2 * gz + q4 * gx) * (0.5f * deltaTime);
    q4 += (q1 * gz + q2 * gy - q3 * gx) * (0.5f * deltaTime);

    // Normalize quaternion
    norm = std::sqrt(q1 * q1 + q2 * q2 + q3 * q3 + q4 * q4);
    norm = 1.0f / norm;
    orientation.w = q1 * norm;
    orientation.x = q2 * norm;
    orientation.y = q3 * norm;
    orientation.z = q4 * norm;
    o.w = q1 * norm;
    o.x = q2 * norm;
    o.y = q3 * norm;
    o.z = q4 * norm;
    LOG_DEBUG(Lib_Pad, "Calculated orientation: {:.2f} {:.2f} {:.2f} {:.2f}", orientation.x,
              orientation.y, orientation.z, orientation.w);
}

void GameController::SetLightBarRGB(u8 r, u8 g, u8 b) {
    if (m_sdl_gamepad != nullptr) {
        SDL_SetGamepadLED(m_sdl_gamepad, r, g, b);
    }
}

bool GameController::SetVibration(u8 smallMotor, u8 largeMotor) {
    if (m_sdl_gamepad != nullptr) {
        return SDL_RumbleGamepad(m_sdl_gamepad, (smallMotor / 255.0f) * 0xFFFF,
                                 (largeMotor / 255.0f) * 0xFFFF, -1);
    }
    return true;
}

void GameController::SetTouchpadState(int touchIndex, bool touchDown, float x, float y) {
    if (touchIndex < 2) {
        std::scoped_lock lock{m_mutex};
        auto state = GetLastState();
        state.time = Libraries::Kernel::sceKernelGetProcessTime();

        state.touchpad[touchIndex].state = touchDown;
        state.touchpad[touchIndex].x = static_cast<u16>(x * 1920);
        state.touchpad[touchIndex].y = static_cast<u16>(y * 941);

        AddState(state);
    }
}

int GameController::GetPadClassFromSDL() {
    if (m_sdl_gamepad) {
        auto joystick = SDL_GetGamepadJoystick(m_sdl_gamepad);
        auto joystick_type = SDL_GetJoystickType(joystick);
        auto joystick_name = SDL_GetJoystickName(joystick);
        switch (joystick_type) {
        case SDL_JOYSTICK_TYPE_GUITAR:
            return 1;
        case SDL_JOYSTICK_TYPE_DRUM_KIT:
            return 2;
        default:
            return 0;
        }
    }
    return 0;
}

bool is_first_check = true;

namespace {
std::string GuidHexForJoystick(SDL_JoystickID id) {
    char buf[33];
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), buf, sizeof(buf));
    return std::string(buf);
}

std::string PathForJoystick(SDL_JoystickID id) {
    const char* p = SDL_GetJoystickPathForID(id);
    return p ? std::string(p) : std::string{};
}

} // namespace

// Pass 1 matches (guid, path) so identical controllers in distinct USB
// ports can be told apart. Pass 2 falls back to GUID-only so a saved
// path-pinned binding still resolves after the kernel renumbered the
// device's event node.
int FindBoundSlotForGamepad(const std::string& guid, const std::string& path) {
    for (int slot = 1; slot <= Config::getNumPlayerSlots(); ++slot) {
        for (const auto& dev : Config::getPlayerSlotDevices(slot)) {
            if (dev.kind != Config::PlayerDeviceKind::Gamepad)
                continue;
            if (!dev.path.empty() && dev.path == path && dev.guid == guid)
                return slot;
        }
    }
    for (int slot = 1; slot <= Config::getNumPlayerSlots(); ++slot) {
        for (const auto& dev : Config::getPlayerSlotDevices(slot)) {
            if (dev.kind != Config::PlayerDeviceKind::Gamepad)
                continue;
            if (dev.guid == guid)
                return slot;
        }
    }
    return -1;
}

namespace {

int FindBoundSlotForGuid(const std::string& guid) {
    return FindBoundSlotForGamepad(guid, std::string{});
}

// Pass-2 placement reserves the slot even if the bound gamepad isn't
// connected yet — keeps an unrelated controller from squatting it.
bool SlotHasGamepadBinding(int slot) {
    for (const auto& dev : Config::getPlayerSlotDevices(slot)) {
        if (dev.kind == Config::PlayerDeviceKind::Gamepad)
            return true;
    }
    return false;
}

void EnableSensorsAndLog(GameController* gc, SDL_Gamepad* pad, int slot) {
    if (SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true)) {
        gc->gyro_poll_rate = SDL_GetGamepadSensorDataRate(pad, SDL_SENSOR_GYRO);
        LOG_INFO(Input, "Gyro initialized for slot {} pad {}: poll rate {}", slot,
                 SDL_GetGamepadID(pad), gc->gyro_poll_rate);
    } else {
        LOG_ERROR(Input, "Failed to enable gyro for slot {} pad {}", slot, SDL_GetGamepadID(pad));
    }
    if (SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true)) {
        gc->accel_poll_rate = SDL_GetGamepadSensorDataRate(pad, SDL_SENSOR_ACCEL);
        LOG_INFO(Input, "Accel initialized for slot {} pad {}: poll rate {}", slot,
                 SDL_GetGamepadID(pad), gc->accel_poll_rate);
    } else {
        LOG_ERROR(Input, "Failed to enable accel for slot {} pad {}", slot, SDL_GetGamepadID(pad));
    }
}

} // namespace

void GameControllers::ApplyAssignmentChanges() {
    using namespace Libraries::UserService;
    auto controllers = *Common::Singleton<GameControllers>::Instance();
    for (int i = 0; i < 4; i++) {
        auto* gc = controllers[i];
        if (gc->m_sdl_gamepad) {
            const SDL_JoystickID id = SDL_GetGamepadID(gc->m_sdl_gamepad);
            const std::string guid = GuidHexForJoystick(id);
            const std::string path = PathForJoystick(id);
            const int bound = FindBoundSlotForGamepad(guid, path);
            if (bound > 0 && bound - 1 != i) {
                LOG_INFO(Input, "Player Assignment changed: slot {} primary moves to slot {}", i,
                         bound - 1);
                SDL_CloseGamepad(gc->m_sdl_gamepad);
                gc->m_sdl_gamepad = nullptr;
                AddUserServiceEvent({OrbisUserServiceEventType::Logout, i + 1});
                gc->user_id = -1;
            }
        }
        auto& secs = gc->m_additional_gamepads;
        secs.erase(std::remove_if(secs.begin(), secs.end(),
                                  [&, i](SDL_Gamepad* p) {
                                      const SDL_JoystickID sid = SDL_GetGamepadID(p);
                                      const std::string guid = GuidHexForJoystick(sid);
                                      const std::string path = PathForJoystick(sid);
                                      const int bound = FindBoundSlotForGamepad(guid, path);
                                      if (bound > 0 && bound - 1 != i) {
                                          SDL_CloseGamepad(p);
                                          return true;
                                      }
                                      return false;
                                  }),
                   secs.end());
    }
    TryOpenSDLControllers(controllers);
}

void GameControllers::PlaceGamepadInSlot(GameControllers& controllers, int slot, SDL_Gamepad* pad,
                                         bool& slot_taken, bool /*fire_login*/) {
    auto* gc = controllers[slot];
    if (!slot_taken) {
        // No auto-Login; EnsureLoggedIn fires from FinalizeUpdate on the
        // first real input, which is what the "press OPTIONS to JOIN"
        // flow gates on.
        gc->m_sdl_gamepad = pad;
        gc->player_index = static_cast<u8>(slot);
        slot_taken = true;
        LOG_INFO(Input, "Gamepad registered for slot {} (primary). Handle: {}", slot,
                 SDL_GetGamepadID(pad));
    } else {
        // SDL events for the secondary route to the same GameController
        // via player_index, so its inputs OR with the primary at
        // m_last_state.
        gc->m_additional_gamepads.push_back(pad);
        LOG_INFO(Input, "Gamepad added to slot {} (secondary). Handle: {}", slot,
                 SDL_GetGamepadID(pad));
    }
    SDL_SetGamepadPlayerIndex(pad, slot);
    EnableSensorsAndLog(gc, pad, slot);
}

void GameControllers::TryOpenSDLControllers(GameControllers& controllers) {
    using namespace Libraries::UserService;
    int controller_count;
    SDL_JoystickID* new_joysticks = SDL_GetGamepads(&controller_count);

    std::unordered_set<SDL_JoystickID> assigned_ids;
    std::array<bool, 4> slot_taken{false, false, false, false};
    std::unordered_set<SDL_JoystickID> connected_set;
    for (int j = 0; j < controller_count; ++j) {
        connected_set.insert(new_joysticks[j]);
    }

    // Disconnect pass — primaries AND secondaries.
    for (int i = 0; i < 4; i++) {
        auto* gc = controllers[i];
        // Primary first.
        if (gc->m_sdl_gamepad) {
            const SDL_JoystickID id = SDL_GetGamepadID(gc->m_sdl_gamepad);
            if (connected_set.count(id)) {
                assigned_ids.insert(id);
                slot_taken[i] = true;
                gc->player_index = static_cast<u8>(i);
            } else {
                SDL_CloseGamepad(gc->m_sdl_gamepad);
                gc->m_sdl_gamepad = nullptr;
                // Promote a secondary to primary if any are still here.
                if (!gc->m_additional_gamepads.empty()) {
                    gc->m_sdl_gamepad = gc->m_additional_gamepads.front();
                    gc->m_additional_gamepads.erase(gc->m_additional_gamepads.begin());
                    slot_taken[i] = true;
                    LOG_INFO(Input,
                             "Slot {} primary disconnected; promoted "
                             "secondary to primary.",
                             i);
                } else {
                    slot_taken[i] = false;
                    AddUserServiceEvent({OrbisUserServiceEventType::Logout, i + 1});
                    gc->user_id = -1;
                }
            }
        }
        // Secondaries — drop any that are gone.
        auto& secs = gc->m_additional_gamepads;
        secs.erase(std::remove_if(secs.begin(), secs.end(),
                                  [&](SDL_Gamepad* p) {
                                      const SDL_JoystickID sid = SDL_GetGamepadID(p);
                                      if (connected_set.count(sid)) {
                                          assigned_ids.insert(sid);
                                          return false;
                                      }
                                      LOG_INFO(Input, "Slot {} secondary disconnected. Handle: {}",
                                               i, sid);
                                      SDL_CloseGamepad(p);
                                      return true;
                                  }),
                   secs.end());
    }

    // Pass 1: route gamepads that have an explicit GUID binding to their
    // bound slot. Pass 2 only places UNBOUND gamepads, so this loop is
    // what makes "I want my Xbox controller on slot 3" actually stick
    // regardless of plug-in order.
    for (int j = 0; j < controller_count; j++) {
        const SDL_JoystickID id = new_joysticks[j];
        if (assigned_ids.contains(id))
            continue;
        const std::string guid = GuidHexForJoystick(id);
        const std::string path = PathForJoystick(id);
        // Path match wins when set; GUID falls back for two-identical-
        // controller setups whose bindings the user pinned to specific
        // USB ports via the picker dialog.
        const int bound = FindBoundSlotForGamepad(guid, path);
        if (bound < 1 || bound > 4)
            continue;
        SDL_Gamepad* pad = SDL_OpenGamepad(id);
        if (!pad)
            continue;
        PlaceGamepadInSlot(controllers, bound - 1, pad, slot_taken[bound - 1], true);
        assigned_ids.insert(id);
    }

    // Pass 2: unbound gamepads land in the first slot that's free AND has
    // no gamepad binding configured (so an unrelated controller can't
    // hijack a slot the user explicitly reserved for a specific device
    // that just hasn't connected yet).
    std::array<bool, 4> reserved{};
    for (int i = 0; i < 4; ++i)
        reserved[i] = SlotHasGamepadBinding(i + 1);
    for (int j = 0; j < controller_count; j++) {
        const SDL_JoystickID id = new_joysticks[j];
        if (assigned_ids.contains(id))
            continue;
        SDL_Gamepad* pad = SDL_OpenGamepad(id);
        if (!pad)
            continue;

        int target = -1;
        for (int i = 0; i < 4; i++) {
            if (slot_taken[i])
                continue;
            if (reserved[i])
                continue;
            target = i;
            break;
        }
        if (target < 0) {
            // Every free slot is reserved for a bound device that hasn't
            // shown up. Fall back to the lowest free slot anyway — the
            // user's bound gamepad can kick us later by reconnecting,
            // but leaving them with no input at all is worse.
            for (int i = 0; i < 4; i++) {
                if (!slot_taken[i]) {
                    target = i;
                    break;
                }
            }
        }
        if (target < 0) {
            // All four slots full — close and ignore.
            SDL_CloseGamepad(pad);
            continue;
        }
        PlaceGamepadInSlot(controllers, target, pad, slot_taken[target], true);
        assigned_ids.insert(id);
    }
    // No auto-Login fallback. is_first_check used to grant Login(1) when
    // no controllers were present at first attach; the new model is "fire
    // Login on first real input", so we drop the fallback to keep
    // behaviour symmetric across all four players.
    is_first_check = false;
}

u32 GameController::Poll() {
    std::scoped_lock lock{m_mutex};
    if (m_connected) {
        auto time = Libraries::Kernel::sceKernelGetProcessTime();
        if (m_states_num == 0) {
            auto diff = (time - m_last_state.time) / 1000;
            if (diff >= 100) {
                AddState(GetLastState());
            }
        } else {
            auto index = (m_first_state - 1 + m_states_num) % MAX_STATES;
            auto diff = (time - m_states[index].time) / 1000;
            if (m_private[index].obtained && diff >= 100) {
                AddState(GetLastState());
            }
        }
    }
    return 100;
}

std::array<std::atomic<u64>, 4> g_last_input_ns{};

void NoteInputOnSlot(int slot) {
    if (slot < 0 || slot >= 4)
        return;
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const u64 ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    g_last_input_ns[slot].store(ns, std::memory_order_relaxed);
}

u64 GetLastInputNs(int slot) {
    if (slot < 0 || slot >= 4)
        return 0;
    return g_last_input_ns[slot].load(std::memory_order_relaxed);
}

void GameControllers::EnsureLoggedIn(int slot) {
    if (slot < 0 || slot >= 4)
        return;
    using namespace Libraries::UserService;
    auto controllers = *Common::Singleton<GameControllers>::Instance();
    auto* gc = controllers[slot];
    if (gc->user_id != static_cast<u32>(-1)) {
        return;
    }
    gc->user_id = slot + 1;
    AddUserServiceEvent({OrbisUserServiceEventType::Login, slot + 1});
    LOG_INFO(Input,
             "Player {} JOIN on first input (slot={}, useSpecialPad={}, class={}, "
             "legacy={})",
             slot + 1, slot, Config::getUseSpecialPad(slot + 1),
             Config::getSpecialPadClass(slot + 1),
             Config::getSpecialPadLegacyPassUSBRawHID(slot + 1));
}

u8 GameControllers::GetGamepadIndexFromJoystickId(SDL_JoystickID id) {
    s32 index = SDL_GetGamepadPlayerIndex(SDL_GetGamepadFromID(id));
    return index;
}

} // namespace Input

namespace GamepadSelect {

int GetDefaultGamepad(SDL_JoystickID* gamepadIDs, int gamepadCount) {
    char GUIDbuf[33];
    if (Config::getDefaultControllerID() != "") {
        for (int i = 0; i < gamepadCount; i++) {
            SDL_GUIDToString(SDL_GetGamepadGUIDForID(gamepadIDs[i]), GUIDbuf, 33);
            std::string currentGUID = std::string(GUIDbuf);
            if (currentGUID == Config::getDefaultControllerID()) {
                return i;
            }
        }
    }
    return -1;
}

int GetIndexfromGUID(SDL_JoystickID* gamepadIDs, int gamepadCount, std::string GUID) {
    char GUIDbuf[33];
    for (int i = 0; i < gamepadCount; i++) {
        SDL_GUIDToString(SDL_GetGamepadGUIDForID(gamepadIDs[i]), GUIDbuf, 33);
        std::string currentGUID = std::string(GUIDbuf);
        if (currentGUID == GUID) {
            return i;
        }
    }
    return -1;
}

std::string GetGUIDString(SDL_JoystickID* gamepadIDs, int index) {
    char GUIDbuf[33];
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(gamepadIDs[index]), GUIDbuf, 33);
    std::string GUID = std::string(GUIDbuf);
    return GUID;
}

std::string GetSelectedGamepad() {
    return SelectedGamepad;
}

void SetSelectedGamepad(std::string GUID) {
    SelectedGamepad = GUID;
}

} // namespace GamepadSelect
