// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <vector>
#include "types.h"

namespace Config {

enum class ConfigMode {
    Default,
    Global,
    Clean,
};
void setConfigMode(ConfigMode mode);

struct GameInstallDir {
    std::filesystem::path path;
    bool enabled;
};

enum HideCursorState : int { Never, Idle, Always };

void load(const std::filesystem::path& path, bool is_game_specific = false);
void save(const std::filesystem::path& path, bool is_game_specific = false);
void resetGameSpecificValue(std::string entry);

int getVolumeSlider();
void setVolumeSlider(int volumeValue, bool is_game_specific = false);
std::string getTrophyKey();
void setTrophyKey(std::string key);
bool getIsFullscreen();
void setIsFullscreen(bool enable, bool is_game_specific = false);
std::string getFullscreenMode();
void setFullscreenMode(std::string mode, bool is_game_specific = false);
std::string getPresentMode();
void setPresentMode(std::string mode, bool is_game_specific = false);
u32 getWindowWidth();
u32 getWindowHeight();
void setWindowWidth(u32 width, bool is_game_specific = false);
void setWindowHeight(u32 height, bool is_game_specific = false);
u32 getInternalScreenWidth();
u32 getInternalScreenHeight();
void setInternalScreenWidth(u32 width);
void setInternalScreenHeight(u32 height);
bool debugDump();
void setDebugDump(bool enable, bool is_game_specific = false);
s32 getGpuId();
void setGpuId(s32 selectedGpuId, bool is_game_specific = false);
bool allowHDR();
void setAllowHDR(bool enable, bool is_game_specific = false);
bool collectShadersForDebug();
void setCollectShaderForDebug(bool enable, bool is_game_specific = false);
bool showSplash();
void setShowSplash(bool enable, bool is_game_specific = false);
std::string sideTrophy();
void setSideTrophy(std::string side, bool is_game_specific = false);
bool nullGpu();
void setNullGpu(bool enable, bool is_game_specific = false);
bool copyGPUCmdBuffers();
void setCopyGPUCmdBuffers(bool enable, bool is_game_specific = false);
bool readbacks();
void setReadbacks(bool enable, bool is_game_specific = false);
bool readbackLinearImages();
void setReadbackLinearImages(bool enable, bool is_game_specific = false);
bool directMemoryAccess();
void setDirectMemoryAccess(bool enable, bool is_game_specific = false);
bool dumpShaders();
void setDumpShaders(bool enable, bool is_game_specific = false);
u32 vblankFreq();
void setVblankFreq(u32 value, bool is_game_specific = false);
bool getisTrophyPopupDisabled();
void setisTrophyPopupDisabled(bool disable, bool is_game_specific = false);
s16 getCursorState();
void setCursorState(s16 cursorState, bool is_game_specific = false);
bool vkValidationEnabled();
void setVkValidation(bool enable, bool is_game_specific = false);
bool vkValidationSyncEnabled();
void setVkSyncValidation(bool enable, bool is_game_specific = false);
bool getVkCrashDiagnosticEnabled();
void setVkCrashDiagnosticEnabled(bool enable, bool is_game_specific = false);
bool getVkHostMarkersEnabled();
void setVkHostMarkersEnabled(bool enable, bool is_game_specific = false);
bool getVkGuestMarkersEnabled();
void setVkGuestMarkersEnabled(bool enable, bool is_game_specific = false);
bool getEnableDiscordRPC();
void setEnableDiscordRPC(bool enable);
bool isRdocEnabled();
void setRdocEnabled(bool enable, bool is_game_specific = false);
std::string getLogType();
void setLogType(const std::string& type, bool is_game_specific = false);
std::string getLogFilter();
void setLogFilter(const std::string& type, bool is_game_specific = false);
double getTrophyNotificationDuration();
void setTrophyNotificationDuration(double newTrophyNotificationDuration,
                                   bool is_game_specific = false);
int getCursorHideTimeout();
void setCursorHideTimeout(int newcursorHideTimeout, bool is_game_specific = false);
std::string getMainOutputDevice();
void setMainOutputDevice(std::string device);
std::string getPadSpkOutputDevice();
void setPadSpkOutputDevice(std::string device);
// Per-player mic config. Slot 0 = primary player; slots 1..3 = harmony
// players for games that open multiple mics (RB4 vocals). The no-arg
// overloads return slot 0 so callers that don't care about per-user
// routing keep working unchanged. getNumMicSlots() returns the array
// length so callers can iterate without hardcoding 4.
int getNumMicSlots();
std::string getMicDevice();
std::string getMicDevice(int slot);
void setMicDevice(std::string device, bool is_game_specific = false);
void setMicDevice(int slot, std::string device, bool is_game_specific = false);
bool getMicGateEnabled();
bool getMicGateEnabled(int slot);
void setMicGateEnabled(bool enabled, bool is_game_specific = false);
void setMicGateEnabled(int slot, bool enabled, bool is_game_specific = false);
int getMicGateThresholdDb();
int getMicGateThresholdDb(int slot);
void setMicGateThresholdDb(int db, bool is_game_specific = false);
void setMicGateThresholdDb(int slot, int db, bool is_game_specific = false);
int getMicGateHoldMs();
void setMicGateHoldMs(int ms, bool is_game_specific = false);
void setSeparateLogFilesEnabled(bool enabled, bool is_game_specific = false);
bool getSeparateLogFilesEnabled();
u32 GetLanguage();
void setLanguage(u32 language, bool is_game_specific = false);
void setUseSpecialPad(int pad, bool use);
bool getUseSpecialPad(int pad);
void setSpecialPadClass(int pad, int type);
int getSpecialPadClass(int pad);
void setSpecialPadLegacyPassUSBRawHID(int pad, bool pass);
bool getSpecialPadLegacyPassUSBRawHID(int pad);

// Per-player device assignment. Each player slot 1..kNumPlayerSlots can
// list any number of input devices that contribute to that slot's pad
// state — a navigation gamepad PLUS a probed RB instrument PLUS a MIDI
// drum module can all be on the same slot, or a single gamepad's slot
// can be overridden to a non-default player. Storage only at this stage;
// the controller open/poll layer still uses first-come-first-served and
// will start consulting this map in a follow-up commit.
constexpr int kNumPlayerSlots = 4;
enum class PlayerDeviceKind {
    Gamepad,  // SDL_Gamepad bound by SDL_JoystickGUID (32 hex chars)
    Kit,      // probed RB instrument bound by VID:PID
    Keyboard, // emulator's virtual keyboard pad (singleton)
    Midi,     // MIDI input port bound by name string
};
struct PlayerDevice {
    PlayerDeviceKind kind = PlayerDeviceKind::Gamepad;
    // For Gamepad: SDL GUID hex (32 chars). For Midi: port name. For Kit
    // / Keyboard: unused.
    std::string guid;
    // For Kit: device VID:PID. Unused for other kinds.
    u16 vid = 0;
    u16 pid = 0;
};
int getNumPlayerSlots();
// Returns the (possibly empty) device list bound to slot 1..kNumPlayerSlots.
// An empty list means "auto" — same as the no-binding default.
std::vector<PlayerDevice> getPlayerSlotDevices(int slot);
void setPlayerSlotDevices(int slot, const std::vector<PlayerDevice>& devices,
                          bool is_game_specific = false);
// Wire-format encoders/decoders used for TOML storage. Exposed so the Qt
// settings UI and the hidtest harness can round-trip device records
// without re-implementing the format.
std::string encodePlayerDevice(const PlayerDevice& dev);
bool decodePlayerDevice(const std::string& encoded, PlayerDevice& out);
bool getPSNSignedIn();
void setPSNSignedIn(bool sign, bool is_game_specific = false);
bool patchShaders(); // no set
bool fpsColor();     // no set
bool isNeoModeConsole();
void setNeoMode(bool enable, bool is_game_specific = false);
bool isDevKitConsole();
void setDevKitConsole(bool enable, bool is_game_specific = false);

bool vkValidationCoreEnabled(); // no set
bool vkValidationGpuEnabled();  // no set
int getExtraDmemInMbytes();
void setExtraDmemInMbytes(int value);
bool getIsMotionControlsEnabled();
void setIsMotionControlsEnabled(bool use, bool is_game_specific = false);
std::string getDefaultControllerID();
void setDefaultControllerID(std::string id);
bool getBackgroundControllerInput();
void setBackgroundControllerInput(bool enable, bool is_game_specific = false);
bool getLoggingEnabled();
void setLoggingEnabled(bool enable, bool is_game_specific = false);
bool getFsrEnabled();
void setFsrEnabled(bool enable, bool is_game_specific = false);
bool getRcasEnabled();
void setRcasEnabled(bool enable, bool is_game_specific = false);
int getRcasAttenuation();
void setRcasAttenuation(int value, bool is_game_specific = false);
bool getIsConnectedToNetwork();
void setConnectedToNetwork(bool enable, bool is_game_specific = false);
void setUserName(const std::string& name, bool is_game_specific = false);
void setChooseHomeTab(const std::string& type, bool is_game_specific = false);
std::filesystem::path getSysModulesPath();
void setSysModulesPath(const std::filesystem::path& path);

// TODO
bool GetLoadGameSizeEnabled();
std::filesystem::path GetSaveDataPath();
void setLoadGameSizeEnabled(bool enable);
bool getCompatibilityEnabled();
bool getCheckCompatibilityOnStartup();
std::string getUserName();
std::string getChooseHomeTab();
bool GetUseUnifiedInputConfig();
void SetUseUnifiedInputConfig(bool use);
bool GetOverrideControllerColor();
void SetOverrideControllerColor(bool enable);
int* GetControllerCustomColor();
void SetControllerCustomColor(int r, int b, int g);
void setGameInstallDirs(const std::vector<std::filesystem::path>& dirs_config);
void setAllGameInstallDirs(const std::vector<GameInstallDir>& dirs_config);
void setSaveDataPath(const std::filesystem::path& path);
void setCompatibilityEnabled(bool use);
void setCheckCompatibilityOnStartup(bool use);
// Gui
bool addGameInstallDir(const std::filesystem::path& dir, bool enabled = true);
void removeGameInstallDir(const std::filesystem::path& dir);
void setGameInstallDirEnabled(const std::filesystem::path& dir, bool enabled);
void setAddonInstallDir(const std::filesystem::path& dir);

const std::vector<std::filesystem::path> getGameInstallDirs();
const std::vector<bool> getGameInstallDirsEnabled();
std::filesystem::path getAddonInstallDir();

void setDefaultValues(bool is_game_specific = false);

constexpr std::string_view GetDefaultGlobalConfig();
std::filesystem::path GetFoolproofInputConfigFile(const std::string& game_id = "");

}; // namespace Config
