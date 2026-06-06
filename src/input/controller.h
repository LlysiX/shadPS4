// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <mutex>
#include <vector>
#include "SDL3/SDL_joystick.h"
#include "common/assert.h"
#include "common/types.h"
#include "core/libraries/pad/pad.h"

struct SDL_Gamepad;

namespace Input {

enum class Axis {
    LeftX = 0,
    LeftY = 1,
    RightX = 2,
    RightY = 3,
    TriggerLeft = 4,
    TriggerRight = 5,

    AxisMax
};

struct TouchpadEntry {
    bool state{};
    u16 x{};
    u16 y{};
};

struct State {
    Libraries::Pad::OrbisPadButtonDataOffset buttonsState{};
    u64 time = 0;
    int axes[static_cast<int>(Axis::AxisMax)] = {128, 128, 128, 128, 0, 0};
    TouchpadEntry touchpad[2] = {{false, 0, 0}, {false, 0, 0}};
    Libraries::Pad::OrbisFVector3 acceleration = {0.0f, 0.0f, 0.0f};
    Libraries::Pad::OrbisFVector3 angularVelocity = {0.0f, 0.0f, 0.0f};
    Libraries::Pad::OrbisFQuaternion orientation = {0.0f, 0.0f, 0.0f, 1.0f};
};

inline int GetAxis(int min, int max, int value) {
    int v = (255 * (value - min)) / (max - min);
    return (v < 0 ? 0 : (v > 255 ? 255 : v));
}

constexpr u32 MAX_STATES = 32;

class GameController {
    friend class GameControllers;

public:
    GameController();
    virtual ~GameController() = default;

    void ReadState(State* state, bool* isConnected, int* connectedCount);
    int ReadStates(State* states, int states_num, bool* isConnected, int* connectedCount);
    State GetLastState() const;
    void CheckButton(int id, Libraries::Pad::OrbisPadButtonDataOffset button, bool isPressed);
    void AddState(const State& state);
    void Axis(int id, Input::Axis axis, int value);
    void Gyro(int id, const float gyro[3]);
    void Acceleration(int id, const float acceleration[3]);
    void SetLightBarRGB(u8 r, u8 g, u8 b);
    bool SetVibration(u8 smallMotor, u8 largeMotor);
    void SetTouchpadState(int touchIndex, bool touchDown, float x, float y);
    int GetPadClassFromSDL();
    u32 Poll();

    float gyro_poll_rate;
    float accel_poll_rate;
    u32 user_id = -1; // ORBIS_USER_SERVICE_USER_ID_INVALID
    static void CalculateOrientation(Libraries::Pad::OrbisFVector3& acceleration,
                                     Libraries::Pad::OrbisFVector3& angularVelocity,
                                     float deltaTime,
                                     Libraries::Pad::OrbisFQuaternion& orientation);

private:
    struct StateInternal {
        bool obtained = false;
    };

    std::mutex m_mutex;
    bool m_connected = true;
    State m_last_state;
    int m_connected_count = 0;
    u32 m_states_num = 0;
    u32 m_first_state = 0;
    std::array<State, MAX_STATES> m_states;
    std::array<StateInternal, MAX_STATES> m_private;

    // m_sdl_gamepad is the "primary" SDL gamepad bound to this slot —
    // queried directly by vibration / lightbar / sensor-poll code.
    // m_additional_gamepads are secondary devices co-bound to the same
    // slot (Config::getPlayerSlotDevices). All of them have player_index
    // set to this slot, so SDL events from any of them route to this
    // GameController via GetGamepadIndexFromJoystickId — their button /
    // axis / sensor updates land in the SAME m_last_state, which gives
    // a free OR-on-digital / last-write-wins-on-analog merge without
    // any explicit aggregation code. The vector is only consulted at
    // open / close time for hot-plug bookkeeping.
    SDL_Gamepad* m_sdl_gamepad = nullptr;
    std::vector<SDL_Gamepad*> m_additional_gamepads;
    u8 player_index = -1;
};

class GameControllers {
    std::array<GameController*, 4> controllers;

public:
    GameControllers()
        : controllers({new GameController(), new GameController(), new GameController(),
                       new GameController()}) {};
    virtual ~GameControllers() = default;
    GameController* operator[](const size_t& i) const {
        if (i > 3) {
            UNREACHABLE_MSG("Index out of bounds for GameControllers!");
        }
        return controllers[i];
    }
    static void TryOpenSDLControllers(GameControllers& controllers);
    static u8 GetGamepadIndexFromJoystickId(SDL_JoystickID id);

    // Close any currently-open gamepad whose GUID is now bound to a
    // different slot than the one it's in, then re-run
    // TryOpenSDLControllers so the placement passes re-route them. Used
    // by the Player Assignment dialog so changes apply without forcing
    // the user to unplug + replug their controllers.
    static void ApplyAssignmentChanges();

private:
    // Attach `pad` to slot `slot` as primary (if the slot is empty) or as
    // a secondary (otherwise). Sets SDL's player_index, enables sensors,
    // and fires a Login event when fire_login=true and the slot was
    // previously empty. Touches GameController's private members, hence
    // a member function rather than a free helper.
    static void PlaceGamepadInSlot(GameControllers& controllers, int slot,
                                   SDL_Gamepad* pad, bool& slot_taken,
                                   bool fire_login);
};

} // namespace Input

namespace GamepadSelect {

int GetIndexfromGUID(SDL_JoystickID* gamepadIDs, int gamepadCount, std::string GUID);
std::string GetGUIDString(SDL_JoystickID* gamepadIDs, int index);
std::string GetSelectedGamepad();
void SetSelectedGamepad(std::string GUID);

} // namespace GamepadSelect
