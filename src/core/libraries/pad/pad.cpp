// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>

#include "common/config.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/libraries/libs.h"
#include "core/libraries/pad/pad_errors.h"
#include "input/controller.h"
#include "input/hid_instrument.h"
#include "pad.h"

namespace Libraries::Pad {

// Map a kit TOML's `device_class` string ("guitar" / "drum" / ...) onto the
// SCE enum. Returns Standard for unknown strings — most kits only set this to
// "guitar" or "drum" in practice.
static OrbisPadDeviceClass KitClassFromString(const std::string& s) {
    if (s == "guitar")
        return OrbisPadDeviceClass::Guitar;
    if (s == "drum")
        return OrbisPadDeviceClass::Drum;
    if (s == "dj_turntable" || s == "turntable")
        return OrbisPadDeviceClass::DjTurntable;
    if (s == "dance_mat" || s == "dancemat")
        return OrbisPadDeviceClass::Dancemat;
    return OrbisPadDeviceClass::Standard;
}

// Resolve the game-visible device class for the given slot. Precedence:
//   1. Legacy raw-HID pass-through is on for this slot AND a kit is active
//      → the kit TOML's `device_class` wins. This is the "Automatic"
//      behaviour surfaced in the Special Devices dialog.
//   2. useSpecialPad{N} → the user-pinned class from Config.
//   3. Otherwise → SDL's detected class (Guitar / Drum / Standard).
OrbisPadDeviceClass ResolveDeviceClass(s32 handle) {
    OrbisPadDeviceClass result;
    if (Config::getSpecialPadLegacyPassUSBRawHID(handle)) {
        const std::string kit_cls = Input::HidInstrument::GetActiveKitDeviceClass(handle);
        if (!kit_cls.empty()) {
            result = KitClassFromString(kit_cls);
            LOG_DEBUG(Lib_Pad, "ResolveDeviceClass handle={} -> {} (kit-toml, cls='{}')",
                      handle, static_cast<int>(result), kit_cls);
            return result;
        }
    }
    if (Config::getUseSpecialPad(handle)) {
        result = (OrbisPadDeviceClass)Config::getSpecialPadClass(handle);
        LOG_DEBUG(Lib_Pad, "ResolveDeviceClass handle={} -> {} (Config::useSpecialPad)",
                  handle, static_cast<int>(result));
        return result;
    }
    auto controllers = *Common::Singleton<Input::GameControllers>::Instance();
    result = (OrbisPadDeviceClass)controllers[handle - 1]->GetPadClassFromSDL();
    LOG_DEBUG(Lib_Pad, "ResolveDeviceClass handle={} -> {} (SDL)", handle,
              static_cast<int>(result));
    return result;
}

int PS4_SYSV_ABI scePadClose(s32 handle) {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadConnectPort() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadDeviceClassGetExtendedInformation(
    s32 handle, OrbisPadDeviceClassExtendedInformation* pExtInfo) {
    LOG_INFO(Lib_Pad, "scePadDeviceClassGetExtendedInformation handle={}", handle);
    std::memset(pExtInfo, 0, sizeof(OrbisPadDeviceClassExtendedInformation));
    pExtInfo->deviceClass = ResolveDeviceClass(handle);
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadDeviceClassParseData(s32 handle, const OrbisPadData* pData,
                                            OrbisPadDeviceClassData* pDeviceClassData) {
    if (!pData || !pDeviceClassData) {
        return ORBIS_PAD_ERROR_INVALID_ARG;
    }
    // Device class follows the shared precedence: legacy raw-HID kit TOML
    // first, then user's per-slot Config, then SDL detection.
    OrbisPadDeviceClass dev_class = ResolveDeviceClass(handle);
    const bool ok = Input::HidInstrument::ParseTypedData(
        handle, pData->deviceUniqueData, pData->deviceUniqueDataLen, dev_class, pDeviceClassData);
    if (!ok) {
        std::memset(pDeviceClassData, 0, sizeof(*pDeviceClassData));
        pDeviceClassData->deviceClass = dev_class;
        pDeviceClassData->bDataValid = false;
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadDeviceOpen() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadDisableVibration() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadDisconnectDevice() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadDisconnectPort() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadEnableAutoDetect() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadEnableExtensionPort() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadEnableSpecificDeviceClass() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadEnableUsbConnection() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetBluetoothAddress() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetCapability() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetControllerInformation(s32 handle, OrbisPadControllerInformation* pInfo) {
    LOG_DEBUG(Lib_Pad, "scePadGetControllerInformation called handle={}", handle);
    if (handle < 0) {
        pInfo->touchPadInfo.pixelDensity = 1;
        pInfo->touchPadInfo.resolution.x = 1920;
        pInfo->touchPadInfo.resolution.y = 950;
        pInfo->stickInfo.deadZoneLeft = 1;
        pInfo->stickInfo.deadZoneRight = 1;
        pInfo->connectionType = ORBIS_PAD_PORT_TYPE_STANDARD;
        pInfo->connectedCount = 1;
        pInfo->connected = false;
        pInfo->deviceClass = OrbisPadDeviceClass::Standard;
        return 0;
    }
    pInfo->touchPadInfo.pixelDensity = 1;
    pInfo->touchPadInfo.resolution.x = 1920;
    pInfo->touchPadInfo.resolution.y = 950;
    pInfo->stickInfo.deadZoneLeft = 1;
    pInfo->stickInfo.deadZoneRight = 1;
    pInfo->connectionType = ORBIS_PAD_PORT_TYPE_STANDARD;
    pInfo->connectedCount = 1;
    pInfo->connected = true;
    pInfo->deviceClass = ResolveDeviceClass(handle);
    if (pInfo->deviceClass != OrbisPadDeviceClass::Standard) {
        pInfo->connectionType = ORBIS_PAD_PORT_TYPE_SPECIAL;
    }
    LOG_DEBUG(Lib_Pad,
              "scePadGetControllerInformation returning handle={} class={} connType={}",
              handle, static_cast<int>(pInfo->deviceClass),
              static_cast<int>(pInfo->connectionType));
    return 0;
}

int PS4_SYSV_ABI scePadGetDataInternal() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetDeviceId() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetDeviceInfo() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetExtControllerInformation(s32 handle,
                                                   OrbisPadExtendedControllerInformation* pInfo) {
    LOG_INFO(Lib_Pad, "called handle = {}", handle);

    pInfo->padType1 = 0;
    pInfo->padType2 = 0;
    pInfo->capability = 0;

    auto res = scePadGetControllerInformation(handle, &pInfo->base);
    return res;
}

int PS4_SYSV_ABI scePadGetExtensionUnitInfo() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetFeatureReport() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetHandle(s32 userId, s32 type, s32 index) {
    if (userId == -1) {
        return ORBIS_PAD_ERROR_DEVICE_NO_HANDLE;
    }
    LOG_DEBUG(Lib_Pad, "(DUMMY) called");
    return 1;
}

int PS4_SYSV_ABI scePadGetIdleCount() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetInfo() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetInfoByPortType() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetLicenseControllerInformation() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetMotionSensorPosition() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetMotionTimerUnit() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetSphereRadius() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadGetVersionInfo() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadInit() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadIsBlasterConnected() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadIsDS4Connected() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadIsLightBarBaseBrightnessControllable() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadIsMoveConnected() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadIsMoveReproductionModel() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadIsValidHandle() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadMbusInit() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadMbusTerm() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadOpen(s32 userId, s32 type, s32 index, const OrbisPadOpenParam* pParam) {
    LOG_INFO(Lib_Pad, "scePadOpen user_id={} type={} index={}", userId, type, index);
    if (userId == -1) {
        return ORBIS_PAD_ERROR_DEVICE_NO_HANDLE;
    }
    const OrbisPadDeviceClass resolved = ResolveDeviceClass(userId);
    const bool special = resolved != OrbisPadDeviceClass::Standard;
    if (special) {
        if (type != ORBIS_PAD_PORT_TYPE_SPECIAL)
            return ORBIS_PAD_ERROR_DEVICE_NOT_CONNECTED;
    } else {
        if (type != ORBIS_PAD_PORT_TYPE_STANDARD && type != ORBIS_PAD_PORT_TYPE_REMOTE_CONTROL)
            return ORBIS_PAD_ERROR_DEVICE_NOT_CONNECTED;
    }
    LOG_INFO(Lib_Pad, "scePadOpen -> user_id={} class={} special={}", userId,
             static_cast<int>(resolved), special);
    scePadResetLightBar(1);
    return userId;
    // todo: using userId as handle works and simplifies some logic,
    // but it's not supposed to be used this way
}

int PS4_SYSV_ABI scePadOpenExt(s32 userId, s32 type, s32 index,
                               const OrbisPadOpenExtParam* pParam) {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    const bool special = ResolveDeviceClass(userId) != OrbisPadDeviceClass::Standard;
    if (special) {
        if (type != ORBIS_PAD_PORT_TYPE_SPECIAL)
            return ORBIS_PAD_ERROR_DEVICE_NOT_CONNECTED;
    } else {
        if (type != ORBIS_PAD_PORT_TYPE_STANDARD && type != ORBIS_PAD_PORT_TYPE_REMOTE_CONTROL)
            return ORBIS_PAD_ERROR_DEVICE_NOT_CONNECTED;
    }
    return userId; // dummy
}

int PS4_SYSV_ABI scePadOpenExt2() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadOutputReport() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

// Fill a single OrbisPadData purely from a legacy instrument's raw HID
// report. Used by scePadRead/scePadReadState when the per-slot
// specialPadLegacyPassUSBRawHID flag is on.
//
// The kit drives the slot's deviceUniqueData (velocity) and its buttons.
// The SDL/keyboard mapping for the slot is ALSO folded in as a fallback so
// menus stay navigable — but via the level-based ReadState (one current
// snapshot per call), never the queue-based ReadStates, so a held key
// cannot be double-counted.
static void FillLegacyInstrumentData(s32 handle, OrbisPadData* pData) {
    auto controllers = *Common::Singleton<Input::GameControllers>::Instance();
    int connectedCount = 0;
    bool isConnected = false;
    Input::State state;
    controllers[handle - 1]->ReadState(&state, &isConnected, &connectedCount);
    const u32 sdl_buttons = static_cast<u32>(state.buttonsState);

    // Sticks / triggers default to "no nav controller bound to this slot":
    // centred sticks, no triggers. For HID-source kits the user can bind
    // a separate gamepad to the same slot (per-player-device-assignment)
    // to navigate menus — in that case let the gamepad's sticks and
    // triggers pass through. For XInput-source kits the SAME physical
    // device is opened twice (once by us, once by hid_instrument's poll
    // loop), so passing SDL sticks would double-count and we keep the
    // safe centred default.
    const std::string kit_source = Input::HidInstrument::GetActiveKitSource(handle);
    // Only XInput-source kits open the same physical SDL gamepad as the
    // navigation pad, so passing SDL state would double-count. HID-source
    // and MIDI-source kits are separate devices — the user's Xbox / DS4
    // sitting beside the drum module is what they expect to navigate
    // menus with, so let its sticks, triggers, and buttons through.
    const bool xinput_source = (kit_source == "xinput");
    const bool nav_passthrough = !xinput_source;
    if (nav_passthrough) {
        pData->leftStick.x =
            static_cast<u8>(state.axes[static_cast<int>(Input::Axis::LeftX)]);
        pData->leftStick.y =
            static_cast<u8>(state.axes[static_cast<int>(Input::Axis::LeftY)]);
        pData->rightStick.x =
            static_cast<u8>(state.axes[static_cast<int>(Input::Axis::RightX)]);
        pData->rightStick.y =
            static_cast<u8>(state.axes[static_cast<int>(Input::Axis::RightY)]);
        pData->analogButtons.l2 =
            static_cast<u8>(state.axes[static_cast<int>(Input::Axis::TriggerLeft)]);
        pData->analogButtons.r2 =
            static_cast<u8>(state.axes[static_cast<int>(Input::Axis::TriggerRight)]);
    } else {
        pData->leftStick.x = 0x80;
        pData->leftStick.y = 0x80;
        pData->rightStick.x = 0x80;
        pData->rightStick.y = 0x80;
        pData->analogButtons.l2 = 0;
        pData->analogButtons.r2 = 0;
    }
    float acc_x = 0.0f, acc_y = 0.0f, acc_z = 0.0f;
    if (Input::HidInstrument::GetLatestAcceleration(handle, acc_x, acc_y, acc_z)) {
        pData->acceleration = {acc_x, acc_y, acc_z};
    } else {
        pData->acceleration = {0.0f, 0.0f, 0.0f};
    }
    pData->angularVelocity = {0.0f, 0.0f, 0.0f};
    pData->orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    pData->touchData = {};
    pData->timestamp = state.time;
    pData->connected = true;
    pData->connectedCount = 1;
    pData->deviceUniqueDataLen = 0;
    // Default: just the keyboard/SDL fallback (kit may not be open yet).
    pData->buttons = static_cast<OrbisPadButtonDataOffset>(sdl_buttons);

    u8 raw[Input::HidInstrument::kMaxRawReport];
    std::size_t raw_len = 0;
    const bool have_kit = Input::HidInstrument::GetLatestReport(handle, raw, &raw_len);
    if (have_kit) {
        // Use the same resolver scePadGetControllerInformation reports
        // through — otherwise a slot on "Automatic" (useSpecialPad=false,
        // kit TOML says drum) hits this branch with cls=Standard and
        // ParseTypedData silently no-ops, leaving the game without
        // velocity bytes.
        const OrbisPadDeviceClass cls = ResolveDeviceClass(handle);
        // For XInput-source kits the SDL gamepad in this slot IS the kit
        // (hid_instrument's poll thread opens the same physical device),
        // so its face / shoulder / D-pad bits would conflict with the
        // kit's PackButtons output (e.g. SDL says Yellow=Square, we say
        // Yellow=Triangle). Mask those.
        // For HID-source kits the SDL gamepad — if any — is a SEPARATE
        // navigation controller bound to the slot via the per-player
        // device assignment (e.g. an Xbox controller next to a MIDI drum
        // module). We don't want a Yellow drum hit to compete with a
        // Triangle press on the gamepad, but we DO want the gamepad's
        // dpad/face buttons to navigate menus. Keep the conservative
        // mask: instrument bits routed only through the kit, navigation
        // bits (Options, Touchpad, Share, PS) through SDL. The dpad
        // stays masked because RB4 treats the kit's HAT decoding as
        // authoritative for the player.
        constexpr u32 kInstrumentBtnMask = static_cast<u32>(
            OrbisPadButtonDataOffset::Square   | OrbisPadButtonDataOffset::Cross    |
            OrbisPadButtonDataOffset::Circle   | OrbisPadButtonDataOffset::Triangle |
            OrbisPadButtonDataOffset::L1       | OrbisPadButtonDataOffset::R1       |
            OrbisPadButtonDataOffset::L2       | OrbisPadButtonDataOffset::R2       |
            OrbisPadButtonDataOffset::Up       | OrbisPadButtonDataOffset::Down     |
            OrbisPadButtonDataOffset::Left     | OrbisPadButtonDataOffset::Right);
        // The instrument-bit mask only applies to XInput-source kits.
        // For HID and MIDI sources the navigation gamepad is a separate
        // device — the user wants pressing X / dpad / etc. to land as
        // Cross / dpad / etc. in the game, not be filtered out as
        // "instrument bits."
        const u32 sdl_for_buttons =
            xinput_source ? (sdl_buttons & ~kInstrumentBtnMask) : sdl_buttons;
        pData->buttons = static_cast<OrbisPadButtonDataOffset>(
            sdl_for_buttons | Input::HidInstrument::PackButtons(handle, raw, raw_len, cls));
        pData->deviceUniqueDataLen = static_cast<u8>(
            Input::HidInstrument::PackDeviceUniqueData(handle, raw, raw_len, cls,
                                                       pData->deviceUniqueData));
    }

}

int PS4_SYSV_ABI scePadRead(s32 handle, OrbisPadData* pData, s32 num) {
    LOG_TRACE(Lib_Pad, "handle: {}", handle);
    if (handle == ORBIS_PAD_ERROR_DEVICE_NO_HANDLE || handle != std::clamp(handle, 1, 4)) {
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    }

    // Legacy-instrument path: this slot is driven solely by the kit's raw
    // HID report. Returns exactly one state; SDL/keyboard read path below
    // is bypassed. Standard controllers are unaffected — they fall through.
    if (Config::getSpecialPadLegacyPassUSBRawHID(handle)) {
        FillLegacyInstrumentData(handle, &pData[0]);
        return 1;
    }

    int connected_count = 0;
    bool connected = false;
    Input::State states[64];
    auto controllers = *Common::Singleton<Input::GameControllers>::Instance();
    int ret_num = controllers[handle - 1]->ReadStates(states, num, &connected, &connected_count);

    if (!connected) {
        ret_num = 1;
    }

    for (int i = 0; i < ret_num; i++) {
        pData[i].buttons = states[i].buttonsState;
        pData[i].leftStick.x = states[i].axes[static_cast<int>(Input::Axis::LeftX)];
        pData[i].leftStick.y = states[i].axes[static_cast<int>(Input::Axis::LeftY)];
        pData[i].rightStick.x = states[i].axes[static_cast<int>(Input::Axis::RightX)];
        pData[i].rightStick.y = states[i].axes[static_cast<int>(Input::Axis::RightY)];
        pData[i].analogButtons.l2 = states[i].axes[static_cast<int>(Input::Axis::TriggerLeft)];
        pData[i].analogButtons.r2 = states[i].axes[static_cast<int>(Input::Axis::TriggerRight)];
        pData[i].acceleration.x = states[i].acceleration.x;
        pData[i].acceleration.y = states[i].acceleration.y;
        pData[i].acceleration.z = states[i].acceleration.z;
        pData[i].angularVelocity.x = states[i].angularVelocity.x;
        pData[i].angularVelocity.y = states[i].angularVelocity.y;
        pData[i].angularVelocity.z = states[i].angularVelocity.z;
        Input::GameController::CalculateOrientation(pData[i].acceleration, pData[i].angularVelocity,
                                                    1.0f / controllers[handle - 1]->gyro_poll_rate,
                                                    pData[i].orientation);
        pData[i].touchData.touchNum =
            (states[i].touchpad[0].state ? 1 : 0) + (states[i].touchpad[1].state ? 1 : 0);
        pData[i].touchData.touch[0].x = states[i].touchpad[0].x;
        pData[i].touchData.touch[0].y = states[i].touchpad[0].y;
        pData[i].touchData.touch[0].id = 1;
        pData[i].touchData.touch[1].x = states[i].touchpad[1].x;
        pData[i].touchData.touch[1].y = states[i].touchpad[1].y;
        pData[i].touchData.touch[1].id = 2;
        pData[i].connected = connected;
        pData[i].timestamp = states[i].time;
        pData[i].connectedCount = connected_count;
        pData[i].deviceUniqueDataLen = 0;
    }

    return ret_num;
}

int PS4_SYSV_ABI scePadReadBlasterForTracker() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadReadExt() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadReadForTracker() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadReadHistory() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadReadState(s32 handle, OrbisPadData* pData) {
    LOG_TRACE(Lib_Pad, "handle: {}", handle);
    if (handle == ORBIS_PAD_ERROR_DEVICE_NO_HANDLE || handle != std::clamp(handle, 1, 4)) {
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    }

    // Legacy-instrument path: same single source of truth as scePadRead.
    if (Config::getSpecialPadLegacyPassUSBRawHID(handle)) {
        FillLegacyInstrumentData(handle, pData);
        return ORBIS_OK;
    }

    auto controllers = *Common::Singleton<Input::GameControllers>::Instance();
    int connectedCount = 0;
    bool isConnected = false;
    Input::State state;
    controllers[handle - 1]->ReadState(&state, &isConnected, &connectedCount);
    pData->buttons = state.buttonsState;
    pData->leftStick.x = state.axes[static_cast<int>(Input::Axis::LeftX)];
    pData->leftStick.y = state.axes[static_cast<int>(Input::Axis::LeftY)];
    pData->rightStick.x = state.axes[static_cast<int>(Input::Axis::RightX)];
    pData->rightStick.y = state.axes[static_cast<int>(Input::Axis::RightY)];
    pData->analogButtons.l2 = state.axes[static_cast<int>(Input::Axis::TriggerLeft)];
    pData->analogButtons.r2 = state.axes[static_cast<int>(Input::Axis::TriggerRight)];
    pData->acceleration.x = state.acceleration.x;
    pData->acceleration.y = state.acceleration.y;
    pData->acceleration.z = state.acceleration.z;
    pData->angularVelocity.x = state.angularVelocity.x;
    pData->angularVelocity.y = state.angularVelocity.y;
    pData->angularVelocity.z = state.angularVelocity.z;
    Input::GameController::CalculateOrientation(pData->acceleration, pData->angularVelocity,
                                                1.0f / controllers[handle - 1]->gyro_poll_rate,
                                                pData->orientation);
    pData->touchData.touchNum =
        (state.touchpad[0].state ? 1 : 0) + (state.touchpad[1].state ? 1 : 0);
    pData->touchData.touch[0].x = state.touchpad[0].x;
    pData->touchData.touch[0].y = state.touchpad[0].y;
    pData->touchData.touch[0].id = 1;
    pData->touchData.touch[1].x = state.touchpad[1].x;
    pData->touchData.touch[1].y = state.touchpad[1].y;
    pData->touchData.touch[1].id = 2;
    pData->timestamp = state.time;
    pData->connected = true;   // isConnected; //TODO fix me proper
    pData->connectedCount = 1; // connectedCount;
    pData->deviceUniqueDataLen = 0;

    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadReadStateExt() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadResetLightBar(s32 handle) {
    LOG_INFO(Lib_Pad, "(DUMMY) called");
    if (handle != 1) {
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    }
    auto controllers = *Common::Singleton<Input::GameControllers>::Instance();
    int* rgb = Config::GetControllerCustomColor();
    controllers[0]->SetLightBarRGB(rgb[0], rgb[1], rgb[2]);
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadResetLightBarAll() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadResetLightBarAllByPortType() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadResetOrientation(s32 handle) {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadResetOrientationForTracker() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetAngularVelocityDeadbandState(s32 handle, bool bEnable) {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetAutoPowerOffCount() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetButtonRemappingInfo() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetConnection() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetExtensionReport() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetFeatureReport() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetForceIntercepted() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetLightBar(s32 handle, const OrbisPadLightBarParam* pParam) {
    if (Config::GetOverrideControllerColor()) {
        return ORBIS_OK;
    }
    if (pParam != nullptr) {
        LOG_DEBUG(Lib_Pad, "called handle = {} rgb = {} {} {}", handle, pParam->r, pParam->g,
                  pParam->b);

        if (pParam->r < 0xD && pParam->g < 0xD && pParam->b < 0xD) {
            LOG_INFO(Lib_Pad, "Invalid lightbar setting");
            return ORBIS_PAD_ERROR_INVALID_LIGHTBAR_SETTING;
        }

        auto* controller = Common::Singleton<Input::GameController>::Instance();
        controller->SetLightBarRGB(pParam->r, pParam->g, pParam->b);
        return ORBIS_OK;
    }
    return ORBIS_PAD_ERROR_INVALID_ARG;
}

int PS4_SYSV_ABI scePadSetLightBarBaseBrightness() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetLightBarBlinking() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetLightBarForTracker() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetLoginUserNumber() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetMotionSensorState(s32 handle, bool bEnable) {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
    // it's already handled by the SDL backend and will be on no matter what
    // (assuming the controller supports it)
}

int PS4_SYSV_ABI scePadSetProcessFocus() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetProcessPrivilege() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetProcessPrivilegeOfButtonRemapping() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetShareButtonMaskForRemotePlay() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetTiltCorrectionState(s32 handle, bool bEnable) {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetUserColor() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetVibration(s32 handle, const OrbisPadVibrationParam* pParam) {
    if (pParam != nullptr) {
        LOG_DEBUG(Lib_Pad, "scePadSetVibration called handle = {} data = {} , {}", handle,
                  pParam->smallMotor, pParam->largeMotor);
        auto* controller = Common::Singleton<Input::GameController>::Instance();
        controller->SetVibration(pParam->smallMotor, pParam->largeMotor);
        return ORBIS_OK;
    }
    return ORBIS_PAD_ERROR_INVALID_ARG;
}

int PS4_SYSV_ABI scePadSetVibrationForce() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSetVrTrackingMode() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadShareOutputData() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadStartRecording() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadStopRecording() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadSwitchConnection() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadVertualDeviceAddDevice() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadVirtualDeviceAddDevice() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadVirtualDeviceDeleteDevice() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadVirtualDeviceDisableButtonRemapping() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadVirtualDeviceGetRemoteSetting() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI scePadVirtualDeviceInsertData() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI Func_28B998C7D8A3DA1D() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI Func_298D21481F94C9FA() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI Func_51E514BCD3A05CA5() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI Func_89C9237E393DA243() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI Func_EF103E845B6F0420() {
    LOG_ERROR(Lib_Pad, "(STUBBED) called");
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("6ncge5+l5Qs", "libScePad", 1, "libScePad", scePadClose);
    LIB_FUNCTION("kazv1NzSB8c", "libScePad", 1, "libScePad", scePadConnectPort);
    LIB_FUNCTION("AcslpN1jHR8", "libScePad", 1, "libScePad",
                 scePadDeviceClassGetExtendedInformation);
    LIB_FUNCTION("IHPqcbc0zCA", "libScePad", 1, "libScePad", scePadDeviceClassParseData);
    LIB_FUNCTION("d7bXuEBycDI", "libScePad", 1, "libScePad", scePadDeviceOpen);
    LIB_FUNCTION("0aziJjRZxqQ", "libScePad", 1, "libScePad", scePadDisableVibration);
    LIB_FUNCTION("pnZXireDoeI", "libScePad", 1, "libScePad", scePadDisconnectDevice);
    LIB_FUNCTION("9ez71nWSvD0", "libScePad", 1, "libScePad", scePadDisconnectPort);
    LIB_FUNCTION("77ooWxGOIVs", "libScePad", 1, "libScePad", scePadEnableAutoDetect);
    LIB_FUNCTION("+cE4Jx431wc", "libScePad", 1, "libScePad", scePadEnableExtensionPort);
    LIB_FUNCTION("E1KEw5XMGQQ", "libScePad", 1, "libScePad", scePadEnableSpecificDeviceClass);
    LIB_FUNCTION("DD-KiRLBqkQ", "libScePad", 1, "libScePad", scePadEnableUsbConnection);
    LIB_FUNCTION("Q66U8FdrMaw", "libScePad", 1, "libScePad", scePadGetBluetoothAddress);
    LIB_FUNCTION("qtasqbvwgV4", "libScePad", 1, "libScePad", scePadGetCapability);
    LIB_FUNCTION("gjP9-KQzoUk", "libScePad", 1, "libScePad", scePadGetControllerInformation);
    LIB_FUNCTION("Uq6LgTJEmQs", "libScePad", 1, "libScePad", scePadGetDataInternal);
    LIB_FUNCTION("hDgisSGkOgw", "libScePad", 1, "libScePad", scePadGetDeviceId);
    LIB_FUNCTION("4rS5zG7RFaM", "libScePad", 1, "libScePad", scePadGetDeviceInfo);
    LIB_FUNCTION("hGbf2QTBmqc", "libScePad", 1, "libScePad",
                 scePadGetExtControllerInformation);
    LIB_FUNCTION("1DmZjZAuzEM", "libScePad", 1, "libScePad", scePadGetExtensionUnitInfo);
    LIB_FUNCTION("PZSoY8j0Pko", "libScePad", 1, "libScePad", scePadGetFeatureReport);
    LIB_FUNCTION("u1GRHp+oWoY", "libScePad", 1, "libScePad", scePadGetHandle);
    LIB_FUNCTION("kiA9bZhbnAg", "libScePad", 1, "libScePad", scePadGetIdleCount);
    LIB_FUNCTION("1Odcw19nADw", "libScePad", 1, "libScePad", scePadGetInfo);
    LIB_FUNCTION("4x5Im8pr0-4", "libScePad", 1, "libScePad", scePadGetInfoByPortType);
    LIB_FUNCTION("vegw8qax5MI", "libScePad", 1, "libScePad",
                 scePadGetLicenseControllerInformation);
    LIB_FUNCTION("WPIB7zBWxVE", "libScePad", 1, "libScePad", scePadGetMotionSensorPosition);
    LIB_FUNCTION("k4+nDV9vbT0", "libScePad", 1, "libScePad", scePadGetMotionTimerUnit);
    LIB_FUNCTION("do-JDWX+zRs", "libScePad", 1, "libScePad", scePadGetSphereRadius);
    LIB_FUNCTION("QuOaoOcSOw0", "libScePad", 1, "libScePad", scePadGetVersionInfo);
    LIB_FUNCTION("hv1luiJrqQM", "libScePad", 1, "libScePad", scePadInit);
    LIB_FUNCTION("bi0WNvZ1nug", "libScePad", 1, "libScePad", scePadIsBlasterConnected);
    LIB_FUNCTION("mEC+xJKyIjQ", "libScePad", 1, "libScePad", scePadIsDS4Connected);
    LIB_FUNCTION("d2Qk-i8wGak", "libScePad", 1, "libScePad",
                 scePadIsLightBarBaseBrightnessControllable);
    LIB_FUNCTION("4y9RNPSBsqg", "libScePad", 1, "libScePad", scePadIsMoveConnected);
    LIB_FUNCTION("9e56uLgk5y0", "libScePad", 1, "libScePad", scePadIsMoveReproductionModel);
    LIB_FUNCTION("pFTi-yOrVeQ", "libScePad", 1, "libScePad", scePadIsValidHandle);
    LIB_FUNCTION("CfwUlQtCFi4", "libScePad", 1, "libScePad", scePadMbusInit);
    LIB_FUNCTION("s7CvzS+9ZIs", "libScePad", 1, "libScePad", scePadMbusTerm);
    LIB_FUNCTION("xk0AcarP3V4", "libScePad", 1, "libScePad", scePadOpen);
    LIB_FUNCTION("WFIiSfXGUq8", "libScePad", 1, "libScePad", scePadOpenExt);
    LIB_FUNCTION("71E9e6n+2R8", "libScePad", 1, "libScePad", scePadOpenExt2);
    LIB_FUNCTION("DrUu8cPrje8", "libScePad", 1, "libScePad", scePadOutputReport);
    LIB_FUNCTION("q1cHNfGycLI", "libScePad", 1, "libScePad", scePadRead);
    LIB_FUNCTION("fm1r2vv5+OU", "libScePad", 1, "libScePad", scePadReadBlasterForTracker);
    LIB_FUNCTION("QjwkT2Ycmew", "libScePad", 1, "libScePad", scePadReadExt);
    LIB_FUNCTION("2NhkFTRnXHk", "libScePad", 1, "libScePad", scePadReadForTracker);
    LIB_FUNCTION("3u4M8ck9vJM", "libScePad", 1, "libScePad", scePadReadHistory);
    LIB_FUNCTION("YndgXqQVV7c", "libScePad", 1, "libScePad", scePadReadState);
    LIB_FUNCTION("5Wf4q349s+Q", "libScePad", 1, "libScePad", scePadReadStateExt);
    LIB_FUNCTION("DscD1i9HX1w", "libScePad", 1, "libScePad", scePadResetLightBar);
    LIB_FUNCTION("+4c9xRLmiXQ", "libScePad", 1, "libScePad", scePadResetLightBarAll);
    LIB_FUNCTION("+Yp6+orqf1M", "libScePad", 1, "libScePad",
                 scePadResetLightBarAllByPortType);
    LIB_FUNCTION("rIZnR6eSpvk", "libScePad", 1, "libScePad", scePadResetOrientation);
    LIB_FUNCTION("jbAqAvLEP4A", "libScePad", 1, "libScePad",
                 scePadResetOrientationForTracker);
    LIB_FUNCTION("r44mAxdSG+U", "libScePad", 1, "libScePad",
                 scePadSetAngularVelocityDeadbandState);
    LIB_FUNCTION("ew647HuKi2Y", "libScePad", 1, "libScePad", scePadSetAutoPowerOffCount);
    LIB_FUNCTION("MbTt1EHYCTg", "libScePad", 1, "libScePad", scePadSetButtonRemappingInfo);
    LIB_FUNCTION("MLA06oNfF+4", "libScePad", 1, "libScePad", scePadSetConnection);
    LIB_FUNCTION("bsbHFI0bl5s", "libScePad", 1, "libScePad", scePadSetExtensionReport);
    LIB_FUNCTION("xqgVCEflEDY", "libScePad", 1, "libScePad", scePadSetFeatureReport);
    LIB_FUNCTION("lrjFx4xWnY8", "libScePad", 1, "libScePad", scePadSetForceIntercepted);
    LIB_FUNCTION("RR4novUEENY", "libScePad", 1, "libScePad", scePadSetLightBar);
    LIB_FUNCTION("dhQXEvmrVNQ", "libScePad", 1, "libScePad", scePadSetLightBarBaseBrightness);
    LIB_FUNCTION("etaQhgPHDRY", "libScePad", 1, "libScePad", scePadSetLightBarBlinking);
    LIB_FUNCTION("iHuOWdvQVpg", "libScePad", 1, "libScePad", scePadSetLightBarForTracker);
    LIB_FUNCTION("o-6Y99a8dKU", "libScePad", 1, "libScePad", scePadSetLoginUserNumber);
    LIB_FUNCTION("clVvL4ZDntw", "libScePad", 1, "libScePad", scePadSetMotionSensorState);
    LIB_FUNCTION("flYYxek1wy8", "libScePad", 1, "libScePad", scePadSetProcessFocus);
    LIB_FUNCTION("DmBx8K+jDWw", "libScePad", 1, "libScePad", scePadSetProcessPrivilege);
    LIB_FUNCTION("FbxEpTRDou8", "libScePad", 1, "libScePad",
                 scePadSetProcessPrivilegeOfButtonRemapping);
    LIB_FUNCTION("yah8Bk4TcYY", "libScePad", 1, "libScePad",
                 scePadSetShareButtonMaskForRemotePlay);
    LIB_FUNCTION("vDLMoJLde8I", "libScePad", 1, "libScePad", scePadSetTiltCorrectionState);
    LIB_FUNCTION("z+GEemoTxOo", "libScePad", 1, "libScePad", scePadSetUserColor);
    LIB_FUNCTION("yFVnOdGxvZY", "libScePad", 1, "libScePad", scePadSetVibration);
    LIB_FUNCTION("8BOObG94-tc", "libScePad", 1, "libScePad", scePadSetVibrationForce);
    LIB_FUNCTION("--jrY4SHfm8", "libScePad", 1, "libScePad", scePadSetVrTrackingMode);
    LIB_FUNCTION("zFJ35q3RVnY", "libScePad", 1, "libScePad", scePadShareOutputData);
    LIB_FUNCTION("80XdmVYsNPA", "libScePad", 1, "libScePad", scePadStartRecording);
    LIB_FUNCTION("gAHvg6JPIic", "libScePad", 1, "libScePad", scePadStopRecording);
    LIB_FUNCTION("Oi7FzRWFr0Y", "libScePad", 1, "libScePad", scePadSwitchConnection);
    LIB_FUNCTION("0MB5x-ieRGI", "libScePad", 1, "libScePad", scePadVertualDeviceAddDevice);
    LIB_FUNCTION("N7tpsjWQ87s", "libScePad", 1, "libScePad", scePadVirtualDeviceAddDevice);
    LIB_FUNCTION("PFec14-UhEQ", "libScePad", 1, "libScePad", scePadVirtualDeviceDeleteDevice);
    LIB_FUNCTION("pjPCronWdxI", "libScePad", 1, "libScePad",
                 scePadVirtualDeviceDisableButtonRemapping);
    LIB_FUNCTION("LKXfw7VJYqg", "libScePad", 1, "libScePad",
                 scePadVirtualDeviceGetRemoteSetting);
    LIB_FUNCTION("IWOyO5jKuZg", "libScePad", 1, "libScePad", scePadVirtualDeviceInsertData);
    LIB_FUNCTION("KLmYx9ij2h0", "libScePad", 1, "libScePad", Func_28B998C7D8A3DA1D);
    LIB_FUNCTION("KY0hSB+Uyfo", "libScePad", 1, "libScePad", Func_298D21481F94C9FA);
    LIB_FUNCTION("UeUUvNOgXKU", "libScePad", 1, "libScePad", Func_51E514BCD3A05CA5);
    LIB_FUNCTION("ickjfjk9okM", "libScePad", 1, "libScePad", Func_89C9237E393DA243);
    LIB_FUNCTION("7xA+hFtvBCA", "libScePad", 1, "libScePad", Func_EF103E845B6F0420);
};

} // namespace Libraries::Pad
