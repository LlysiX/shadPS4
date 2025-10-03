// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "usbd.h"

namespace Libraries::Usbd {

int PS4_SYSV_ABI sceUsbdAllocTransfer() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdAttachKernelDriver() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdBulkTransfer() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdCancelTransfer() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdCheckConnected() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdClaimInterface() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdClearHalt() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdClose() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdControlTransfer() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdControlTransferGetData() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdControlTransferGetSetup() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdDetachKernelDriver() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdEventHandlerActive() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdEventHandlingOk() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdExit() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdFillBulkTransfer() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdFillControlSetup() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdFillControlTransfer() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdFillInterruptTransfer() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdFillIsoTransfer() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdFreeConfigDescriptor() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdFreeDeviceList() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdFreeTransfer() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdGetActiveConfigDescriptor() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdGetBusNumber() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdGetConfigDescriptor() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdGetConfigDescriptorByValue() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdGetConfiguration() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdGetDescriptor() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdGetDevice() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdGetDeviceAddress() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdGetDeviceDescriptor() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdGetDeviceList() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdGetDeviceSpeed() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdGetIsoPacketBuffer() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdGetMaxIsoPacketSize() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdGetMaxPacketSize() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdGetStringDescriptor() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdGetStringDescriptorAscii() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdHandleEvents() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdHandleEventsLocked() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdHandleEventsTimeout() {
    LOG_DEBUG(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdInit() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return 0x80240005; // Skip
}

int PS4_SYSV_ABI sceUsbdInterruptTransfer() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdKernelDriverActive() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdLockEvents() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdLockEventWaiters() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdOpen() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdOpenDeviceWithVidPid() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdRefDevice() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdReleaseInterface() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdResetDevice() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdSetConfiguration() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdSetInterfaceAltSetting() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdSetIsoPacketLengths() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdSubmitTransfer() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdTryLockEvents() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdUnlockEvents() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdUnlockEventWaiters() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdUnrefDevice() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceUsbdWaitForEvent() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI Func_65F6EF33E38FFF50() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI Func_97F056BAD90AADE7() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI Func_C55104A33B35B264() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI Func_D56B43060720B1E0() {
    LOG_ERROR(Lib_Usbd, "(STUBBED) called");
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("0ktE1PhzGFU", "libSceUsbd", 1, "libSceUsbd", sceUsbdAllocTransfer);
    LIB_FUNCTION("BKMEGvfCPyU", "libSceUsbd", 1, "libSceUsbd", sceUsbdAttachKernelDriver);
    LIB_FUNCTION("fotb7DzeHYw", "libSceUsbd", 1, "libSceUsbd", sceUsbdBulkTransfer);
    LIB_FUNCTION("-KNh1VFIzlM", "libSceUsbd", 1, "libSceUsbd", sceUsbdCancelTransfer);
    LIB_FUNCTION("MlW6deWfPp0", "libSceUsbd", 1, "libSceUsbd", sceUsbdCheckConnected);
    LIB_FUNCTION("AE+mHBHneyk", "libSceUsbd", 1, "libSceUsbd", sceUsbdClaimInterface);
    LIB_FUNCTION("3tPPMo4QRdY", "libSceUsbd", 1, "libSceUsbd", sceUsbdClearHalt);
    LIB_FUNCTION("HarYYlaFGJY", "libSceUsbd", 1, "libSceUsbd", sceUsbdClose);
    LIB_FUNCTION("RRKFcKQ1Ka4", "libSceUsbd", 1, "libSceUsbd", sceUsbdControlTransfer);
    LIB_FUNCTION("XUWtxI31YEY", "libSceUsbd", 1, "libSceUsbd", sceUsbdControlTransferGetData);
    LIB_FUNCTION("SEdQo8CFmus", "libSceUsbd", 1, "libSceUsbd", sceUsbdControlTransferGetSetup);
    LIB_FUNCTION("Y5go+ha6eDs", "libSceUsbd", 1, "libSceUsbd", sceUsbdDetachKernelDriver);
    LIB_FUNCTION("Vw8Hg1CN028", "libSceUsbd", 1, "libSceUsbd", sceUsbdEventHandlerActive);
    LIB_FUNCTION("e7gp1xhu6RI", "libSceUsbd", 1, "libSceUsbd", sceUsbdEventHandlingOk);
    LIB_FUNCTION("Fq6+0Fm55xU", "libSceUsbd", 1, "libSceUsbd", sceUsbdExit);
    LIB_FUNCTION("oHCade-0qQ0", "libSceUsbd", 1, "libSceUsbd", sceUsbdFillBulkTransfer);
    LIB_FUNCTION("8KrqbaaPkE0", "libSceUsbd", 1, "libSceUsbd", sceUsbdFillControlSetup);
    LIB_FUNCTION("7VGfMerK6m0", "libSceUsbd", 1, "libSceUsbd", sceUsbdFillControlTransfer);
    LIB_FUNCTION("t3J5pXxhJlI", "libSceUsbd", 1, "libSceUsbd", sceUsbdFillInterruptTransfer);
    LIB_FUNCTION("xqmkjHCEOSY", "libSceUsbd", 1, "libSceUsbd", sceUsbdFillIsoTransfer);
    LIB_FUNCTION("Hvd3S--n25w", "libSceUsbd", 1, "libSceUsbd", sceUsbdFreeConfigDescriptor);
    LIB_FUNCTION("EQ6SCLMqzkM", "libSceUsbd", 1, "libSceUsbd", sceUsbdFreeDeviceList);
    LIB_FUNCTION("-sgi7EeLSO8", "libSceUsbd", 1, "libSceUsbd", sceUsbdFreeTransfer);
    LIB_FUNCTION("S1o1C6yOt5g", "libSceUsbd", 1, "libSceUsbd", sceUsbdGetActiveConfigDescriptor);
    LIB_FUNCTION("t7WE9mb1TB8", "libSceUsbd", 1, "libSceUsbd", sceUsbdGetBusNumber);
    LIB_FUNCTION("Dkm5qe8j3XE", "libSceUsbd", 1, "libSceUsbd", sceUsbdGetConfigDescriptor);
    LIB_FUNCTION("GQsAVJuy8gM", "libSceUsbd", 1, "libSceUsbd", sceUsbdGetConfigDescriptorByValue);
    LIB_FUNCTION("L7FoTZp3bZs", "libSceUsbd", 1, "libSceUsbd", sceUsbdGetConfiguration);
    LIB_FUNCTION("-JBoEtvTxvA", "libSceUsbd", 1, "libSceUsbd", sceUsbdGetDescriptor);
    LIB_FUNCTION("rsl9KQ-agyA", "libSceUsbd", 1, "libSceUsbd", sceUsbdGetDevice);
    LIB_FUNCTION("GjlCrU4GcIY", "libSceUsbd", 1, "libSceUsbd", sceUsbdGetDeviceAddress);
    LIB_FUNCTION("bhomgbiQgeo", "libSceUsbd", 1, "libSceUsbd", sceUsbdGetDeviceDescriptor);
    LIB_FUNCTION("8qB9Ar4P5nc", "libSceUsbd", 1, "libSceUsbd", sceUsbdGetDeviceList);
    LIB_FUNCTION("e1UWb8cWPJM", "libSceUsbd", 1, "libSceUsbd", sceUsbdGetDeviceSpeed);
    LIB_FUNCTION("vokkJ0aDf54", "libSceUsbd", 1, "libSceUsbd", sceUsbdGetIsoPacketBuffer);
    LIB_FUNCTION("nuIRlpbxauM", "libSceUsbd", 1, "libSceUsbd", sceUsbdGetMaxIsoPacketSize);
    LIB_FUNCTION("YJ0cMAlLuxQ", "libSceUsbd", 1, "libSceUsbd", sceUsbdGetMaxPacketSize);
    LIB_FUNCTION("g2oYm1DitDg", "libSceUsbd", 1, "libSceUsbd", sceUsbdGetStringDescriptor);
    LIB_FUNCTION("t4gUfGsjk+g", "libSceUsbd", 1, "libSceUsbd", sceUsbdGetStringDescriptorAscii);
    LIB_FUNCTION("EkqGLxWC-S0", "libSceUsbd", 1, "libSceUsbd", sceUsbdHandleEvents);
    LIB_FUNCTION("rt-WeUGibfg", "libSceUsbd", 1, "libSceUsbd", sceUsbdHandleEventsLocked);
    LIB_FUNCTION("+wU6CGuZcWk", "libSceUsbd", 1, "libSceUsbd", sceUsbdHandleEventsTimeout);
    LIB_FUNCTION("TOhg7P6kTH4", "libSceUsbd", 1, "libSceUsbd", sceUsbdInit);
    LIB_FUNCTION("rxi1nCOKWc8", "libSceUsbd", 1, "libSceUsbd", sceUsbdInterruptTransfer);
    LIB_FUNCTION("RLf56F-WjKQ", "libSceUsbd", 1, "libSceUsbd", sceUsbdKernelDriverActive);
    LIB_FUNCTION("u9yKks02-rA", "libSceUsbd", 1, "libSceUsbd", sceUsbdLockEvents);
    LIB_FUNCTION("AeGaY8JrAV4", "libSceUsbd", 1, "libSceUsbd", sceUsbdLockEventWaiters);
    LIB_FUNCTION("VJ6oMq-Di2U", "libSceUsbd", 1, "libSceUsbd", sceUsbdOpen);
    LIB_FUNCTION("vrQXYRo1Gwk", "libSceUsbd", 1, "libSceUsbd", sceUsbdOpenDeviceWithVidPid);
    LIB_FUNCTION("U1t1SoJvV-A", "libSceUsbd", 1, "libSceUsbd", sceUsbdRefDevice);
    LIB_FUNCTION("REfUTmTchMw", "libSceUsbd", 1, "libSceUsbd", sceUsbdReleaseInterface);
    LIB_FUNCTION("hvMn0QJXj5g", "libSceUsbd", 1, "libSceUsbd", sceUsbdResetDevice);
    LIB_FUNCTION("FhU9oYrbXoA", "libSceUsbd", 1, "libSceUsbd", sceUsbdSetConfiguration);
    LIB_FUNCTION("DVCQW9o+ki0", "libSceUsbd", 1, "libSceUsbd", sceUsbdSetInterfaceAltSetting);
    LIB_FUNCTION("dJxro8Nzcjk", "libSceUsbd", 1, "libSceUsbd", sceUsbdSetIsoPacketLengths);
    LIB_FUNCTION("L0EHgZZNVas", "libSceUsbd", 1, "libSceUsbd", sceUsbdSubmitTransfer);
    LIB_FUNCTION("TcXVGc-LPbQ", "libSceUsbd", 1, "libSceUsbd", sceUsbdTryLockEvents);
    LIB_FUNCTION("RA2D9rFH-Uw", "libSceUsbd", 1, "libSceUsbd", sceUsbdUnlockEvents);
    LIB_FUNCTION("1DkGvUQYFKI", "libSceUsbd", 1, "libSceUsbd", sceUsbdUnlockEventWaiters);
    LIB_FUNCTION("OULgIo1zAsA", "libSceUsbd", 1, "libSceUsbd", sceUsbdUnrefDevice);
    LIB_FUNCTION("ys2e9VRBPrY", "libSceUsbd", 1, "libSceUsbd", sceUsbdWaitForEvent);
    LIB_FUNCTION("ZfbvM+OP-1A", "libSceUsbd", 1, "libSceUsbd", Func_65F6EF33E38FFF50);
    LIB_FUNCTION("l-BWutkKrec", "libSceUsbd", 1, "libSceUsbd", Func_97F056BAD90AADE7);
    LIB_FUNCTION("xVEEozs1smQ", "libSceUsbd", 1, "libSceUsbd", Func_C55104A33B35B264);
    LIB_FUNCTION("1WtDBgcgseA", "libSceUsbd", 1, "libSceUsbd", Func_D56B43060720B1E0);
};

} // namespace Libraries::Usbd
