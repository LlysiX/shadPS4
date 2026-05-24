// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Host-side raw-HID passthrough for legacy PS3 instruments (drum kits,
// guitars) used with games that read instrument data via the PS4
// libScePad legacy channel (OrbisPadData::deviceUniqueData).
//
// When Config::getSpecialPadLegacyPassUSBRawHID(slot) is true for a player
// slot, we open the kit by USB VID:PID with SDL_hid_*, poll it on a
// background thread, and the latest input report is packed into the slot's
// deviceUniqueData buffer in scePadRead / scePadReadState. The declared
// device class (specialPadClass*) is *not* touched; this only changes the
// per-frame instrument payload.

#pragma once

#include <cstddef>
#include <string>
#include "common/types.h"
#include "core/libraries/pad/pad.h"

namespace Input::HidInstrument {

constexpr int kNumSlots = 4;
constexpr std::size_t kMaxRawReport = 64;
constexpr std::size_t kMaxDeviceUniqueData =
    Libraries::Pad::ORBIS_PAD_MAX_DEVICE_UNIQUE_DATA_SIZE;

// Idempotent. Called lazily on first use; safe to call from any thread.
bool EnsureInit();

// Re-scan scripts/kits/ and the user kits dir, picking up any new TOML
// definitions. Existing open device handles are kept; the poll thread will
// open or close devices on the next tick if config flags or kit availability
// changed.
void RescanKits();

// Stop the background thread, close all opened HID handles. Called on exit.
void Shutdown();

// Copy the latest raw report for `slot` (1..4) into `out` (size >= kMaxRawReport).
// Returns false if the slot has the flag disabled, no device is open, or no
// report has been received yet.
bool GetLatestReport(int slot, u8* out, std::size_t* out_len);

// Get the decoded acceleration vector (X, Y, Z) for the active legacy
// instrument in `slot` (1..4). Used to pass guitar tilt to
// scePadRead/scePadReadState. Returns false if no kit/tilt is active.
bool GetLatestAcceleration(int slot, float& out_x, float& out_y, float& out_z);

// Pack a kit's raw HID report into the 12-byte deviceUniqueData wire format
// that scePadRead/scePadReadState will deliver to the game. Single source of
// truth for the byte layout; if RB4 misreads, edit the table in the .cpp.
// Returns the number of bytes written (1..kMaxDeviceUniqueData), or 0 if the
// raw report is too short.
std::size_t PackDeviceUniqueData(int slot, const u8* raw, std::size_t raw_len,
                                 Libraries::Pad::OrbisPadDeviceClass dev_class,
                                 u8 out[kMaxDeviceUniqueData]);

// Translate the kit's digital state (face-button bits, Start/Select/PS,
// D-pad HAT) into an OrbisPadButtonDataOffset bitmap suitable for
// OR-ing into OrbisPadData::buttons. Lets the game see menu inputs
// (Options/Touchpad/dpad) when the kit isn't a recognized SDL gamepad.
u32 PackButtons(int slot, const u8* raw, std::size_t raw_len,
                Libraries::Pad::OrbisPadDeviceClass dev_class);

// Decode the packed deviceUniqueData buffer into a typed
// OrbisPadDeviceClassData (drum/guitar). `slot` is the player slot (1..4)
// used to look up the kit's tilt/accel state for the guitar fields.
// Returns true on success.
bool ParseTypedData(int slot, const u8* dud, std::size_t dud_len,
                    Libraries::Pad::OrbisPadDeviceClass dev_class,
                    Libraries::Pad::OrbisPadDeviceClassData* out);

// Inspection helpers for the Qt UI.
std::string GetActiveKitName(int slot);  // empty if no kit open
std::size_t GetLoadedKitCount();         // for "N kits loaded" status

// True if any player slot has legacy raw-HID enabled AND we have a kit
// definition matching the given VID:PID. Used by libSceUsbd to hide kits
// from the game so the guitar/drum doesn't appear twice (once through the
// HID passthrough, once through direct USB enumeration).
bool ShouldHideFromUsbd(u16 vid, u16 pid);

}  // namespace Input::HidInstrument
