// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <thread>
#include <boost/asio/io_context.hpp>

#include "common/assert.h"
#include "common/debug.h"
#include "common/logging/log.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "common/va_ctx.h"
#include "core/file_sys/fs.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/kernel/equeue.h"
#include "core/libraries/kernel/file_system.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/memory.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/kernel/threads.h"
#include "core/libraries/kernel/threads/exception.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/libs.h"

#ifdef _WIN64
#include <Rpc.h>
#endif
#include <common/singleton.h>
#include <core/libraries/network/net_error.h>
#include <core/libraries/network/sockets.h>
#include <core/linker.h>
#include "aio.h"

namespace Libraries::Kernel {

static u64 g_stack_chk_guard = 0xDEADBEEF54321ABC; // dummy return

boost::asio::io_context io_context;
static std::mutex m_asio_req;
static std::condition_variable_any cv_asio_req;
static std::atomic<u32> asio_requests;
static std::jthread service_thread;

Core::EntryParams entry_params{};

void KernelSignalRequest() {
    std::unique_lock lock{m_asio_req};
    ++asio_requests;
    cv_asio_req.notify_one();
}

static void KernelServiceThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:Kernel_ServiceThread");

    while (!stoken.stop_requested()) {
        HLE_TRACE;
        {
            std::unique_lock lock{m_asio_req};
            Common::CondvarWait(cv_asio_req, lock, stoken, [] { return asio_requests != 0; });
        }
        if (stoken.stop_requested()) {
            break;
        }

        io_context.run();
        io_context.reset();

        asio_requests = 0;
    }
}

static PS4_SYSV_ABI void stack_chk_fail() {
    UNREACHABLE();
}

static thread_local int g_posix_errno = 0;

int* PS4_SYSV_ABI __Error() {
    return &g_posix_errno;
}

void ErrSceToPosix(int error) {
    g_posix_errno = error - ORBIS_KERNEL_ERROR_UNKNOWN;
}

int ErrnoToSceKernelError(int error) {
    return error + ORBIS_KERNEL_ERROR_UNKNOWN;
}

void SetPosixErrno(int e) {
    // Some error numbers are different between supported OSes or the PS4
    switch (e) {
    case EPERM:
        g_posix_errno = POSIX_EPERM;
        break;
    case EAGAIN:
        g_posix_errno = POSIX_EAGAIN;
        break;
    case ENOMEM:
        g_posix_errno = POSIX_ENOMEM;
        break;
    case EINVAL:
        g_posix_errno = POSIX_EINVAL;
        break;
    case ENOSPC:
        g_posix_errno = POSIX_ENOSPC;
        break;
    case ERANGE:
        g_posix_errno = POSIX_ERANGE;
        break;
    case EDEADLK:
        g_posix_errno = POSIX_EDEADLK;
        break;
    case ETIMEDOUT:
        g_posix_errno = POSIX_ETIMEDOUT;
        break;
    default:
        g_posix_errno = e;
    }
}

static uint64_t g_mspace_atomic_id_mask = 0;
static uint64_t g_mstate_table[64] = {0};

struct HeapInfoInfo {
    uint64_t size = sizeof(HeapInfoInfo);
    uint32_t flag;
    uint32_t getSegmentInfo;
    uint64_t* mspace_atomic_id_mask;
    uint64_t* mstate_table;
};

void PS4_SYSV_ABI sceLibcHeapGetTraceInfo(HeapInfoInfo* info) {
    info->mspace_atomic_id_mask = &g_mspace_atomic_id_mask;
    info->mstate_table = g_mstate_table;
    info->getSegmentInfo = 0;
}

s64 PS4_SYSV_ABI ps4__write(int d, const char* buf, std::size_t nbytes) {
    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    auto* file = h->GetFile(d);
    if (file == nullptr) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }
    std::scoped_lock lk{file->m_mutex};
    if (file->type == Core::FileSys::FileType::Device) {
        return file->device->write(buf, nbytes);
    }
    return file->f.WriteRaw<u8>(buf, nbytes);
}

s64 PS4_SYSV_ABI ps4__read(int d, void* buf, u64 nbytes) {
    if (d == 0) {
        return static_cast<s64>(
            strlen(std::fgets(static_cast<char*>(buf), static_cast<int>(nbytes), stdin)));
    }
    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    auto* file = h->GetFile(d);
    if (file == nullptr) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }
    std::scoped_lock lk{file->m_mutex};
    if (file->type == Core::FileSys::FileType::Device) {
        return file->device->read(buf, nbytes);
    }
    return file->f.ReadRaw<u8>(buf, nbytes);
}

struct OrbisKernelUuid {
    u32 timeLow;
    u16 timeMid;
    u16 timeHiAndVersion;
    u8 clockSeqHiAndReserved;
    u8 clockSeqLow;
    u8 node[6];
};

int PS4_SYSV_ABI sceKernelUuidCreate(OrbisKernelUuid* orbisUuid) {
#ifdef _WIN64
    UUID uuid;
    UuidCreate(&uuid);
    orbisUuid->timeLow = uuid.Data1;
    orbisUuid->timeMid = uuid.Data2;
    orbisUuid->timeHiAndVersion = uuid.Data3;
    orbisUuid->clockSeqHiAndReserved = uuid.Data4[0];
    orbisUuid->clockSeqLow = uuid.Data4[1];
    for (int i = 0; i < 6; i++) {
        orbisUuid->node[i] = uuid.Data4[2 + i];
    }
#else
    LOG_ERROR(Kernel, "sceKernelUuidCreate: Add linux");
#endif
    return 0;
}

int PS4_SYSV_ABI kernel_ioctl(int fd, u64 cmd, VA_ARGS) {
    auto* h = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    auto* file = h->GetFile(fd);
    if (file == nullptr) {
        LOG_INFO(Lib_Kernel, "ioctl: fd = {:X} cmd = {:X} file == nullptr", fd, cmd);
        g_posix_errno = POSIX_EBADF;
        return -1;
    }
    if (file->type != Core::FileSys::FileType::Device) {
        LOG_WARNING(Lib_Kernel, "ioctl: fd = {:X} cmd = {:X} file->type != Device", fd, cmd);
        g_posix_errno = ENOTTY;
        return -1;
    }
    VA_CTX(ctx);
    int result = file->device->ioctl(cmd, &ctx);
    LOG_TRACE(Lib_Kernel, "ioctl: fd = {:X} cmd = {:X} result = {}", fd, cmd, result);
    if (result < 0) {
        ErrSceToPosix(result);
        return -1;
    }
    return result;
}

const char* PS4_SYSV_ABI sceKernelGetFsSandboxRandomWord() {
    const char* path = "sys";
    return path;
}

s32 PS4_SYSV_ABI _sigprocmask() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI posix_getpagesize() {
    return 16_KB;
}

// stubbed on non-devkit consoles
s32 PS4_SYSV_ABI sceKernelGetGPI() {
    LOG_DEBUG(Kernel, "called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI posix_getpagesize() {
    return 16_KB;
}

s32 PS4_SYSV_ABI getargc() {
    return entry_params.argc;
}

const char** PS4_SYSV_ABI getargv() {
    return entry_params.argv;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    service_thread = std::jthread{KernelServiceThread};

    Libraries::Kernel::RegisterFileSystem(sym);
    Libraries::Kernel::RegisterTime(sym);
    Libraries::Kernel::RegisterThreads(sym);
    Libraries::Kernel::RegisterKernelEventFlag(sym);
    Libraries::Kernel::RegisterMemory(sym);
    Libraries::Kernel::RegisterEventQueue(sym);
    Libraries::Kernel::RegisterProcess(sym);
    Libraries::Kernel::RegisterException(sym);

    LIB_OBJ("f7uOxY9mM1U", "libkernel", 1, "libkernel", &g_stack_chk_guard);
    LIB_FUNCTION("D4yla3vx4tY", "libkernel", 1, "libkernel", sceKernelError);
    LIB_FUNCTION("Mv1zUObHvXI", "libkernel", 1, "libkernel", sceKernelGetSystemSwVersion);
    LIB_FUNCTION("PfccT7qURYE", "libkernel", 1, "libkernel", kernel_ioctl);
    LIB_FUNCTION("wW+k21cmbwQ", "libkernel", 1, "libkernel", kernel_ioctl);
    LIB_FUNCTION("JGfTMBOdUJo", "libkernel", 1, "libkernel", sceKernelGetFsSandboxRandomWord);
    LIB_FUNCTION("6xVpy0Fdq+I", "libkernel", 1, "libkernel", _sigprocmask);
    LIB_FUNCTION("Xjoosiw+XPI", "libkernel", 1, "libkernel", sceKernelUuidCreate);
    LIB_FUNCTION("Ou3iL1abvng", "libkernel", 1, "libkernel", stack_chk_fail);
    LIB_FUNCTION("9BcDykPmo1I", "libkernel", 1, "libkernel", __Error);
    LIB_FUNCTION("k+AXqu2-eBc", "libkernel", 1, "libkernel", posix_getpagesize);
    LIB_FUNCTION("k+AXqu2-eBc", "libScePosix", 1, "libkernel", posix_getpagesize);
    LIB_FUNCTION("NWtTN10cJzE", "libSceLibcInternalExt", 1, "libSceLibcInternal",
                 sceLibcHeapGetTraceInfo);

    // network
    LIB_FUNCTION("XVL8So3QJUk", "libkernel", 1, "libkernel", Libraries::Net::sys_connect);
    LIB_FUNCTION("pG70GT5yRo4", "libkernel", 1, "libkernel", Libraries::Net::sys_socketex);
    LIB_FUNCTION("KuOmgKoqCdY", "libkernel", 1, "libkernel", Libraries::Net::sys_bind);
    LIB_FUNCTION("6O8EwYOgH9Y", "libkernel", 1, "libkernel", Libraries::Net::sys_getsockopt);
    LIB_FUNCTION("fFxGkxF2bVo", "libkernel", 1, "libkernel", Libraries::Net::sys_setsockopt);
    LIB_FUNCTION("pxnCmagrtao", "libkernel", 1, "libkernel", Libraries::Net::sys_listen);
    LIB_FUNCTION("3e+4Iv7IJ8U", "libkernel", 1, "libkernel", Libraries::Net::sys_accept);
    LIB_FUNCTION("TUuiYS2kE8s", "libkernel", 1, "libkernel", Libraries::Net::sys_shutdown);
    LIB_FUNCTION("TU-d9PfIHPM", "libkernel", 1, "libkernel", Libraries::Net::sys_socket);
    LIB_FUNCTION("MZb0GKT3mo8", "libkernel", 1, "libkernel", Libraries::Net::sys_socketpair);
    LIB_FUNCTION("MZb0GKT3mo8", "libkernel_ps2emu", 1, "libkernel", Libraries::Net::sys_socketpair);
    LIB_FUNCTION("K1S8oc61xiM", "libkernel", 1, "libkernel", Libraries::Net::sys_htonl);
    LIB_FUNCTION("jogUIsOV3-U", "libkernel", 1, "libkernel", Libraries::Net::sys_htons);
    LIB_FUNCTION("fZOeZIOEmLw", "libkernel", 1, "libkernel", Libraries::Net::sys_send);
    LIB_FUNCTION("oBr313PppNE", "libkernel", 1, "libkernel", Libraries::Net::sys_sendto);
    LIB_FUNCTION("Ez8xjo9UF4E", "libkernel", 1, "libkernel", Libraries::Net::sys_recv);
    LIB_FUNCTION("lUk6wrGXyMw", "libkernel", 1, "libkernel", Libraries::Net::sys_recvfrom);

    LIB_FUNCTION("TU-d9PfIHPM", "libScePosix", 1, "libkernel", Libraries::Net::sys_socket);
    LIB_FUNCTION("fZOeZIOEmLw", "libScePosix", 1, "libkernel", Libraries::Net::sys_send);
    LIB_FUNCTION("oBr313PppNE", "libScePosix", 1, "libkernel", Libraries::Net::sys_sendto);
    LIB_FUNCTION("Ez8xjo9UF4E", "libScePosix", 1, "libkernel", Libraries::Net::sys_recv);
    LIB_FUNCTION("lUk6wrGXyMw", "libScePosix", 1, "libkernel", Libraries::Net::sys_recvfrom);
    LIB_FUNCTION("hI7oVeOluPM", "libScePosix", 1, "libkernel", Libraries::Net::sys_recvmsg);
    LIB_FUNCTION("TXFFFiNldU8", "libScePosix", 1, "libkernel", Libraries::Net::sys_getpeername);
    LIB_FUNCTION("6O8EwYOgH9Y", "libScePosix", 1, "libkernel", Libraries::Net::sys_getsockopt);
    LIB_FUNCTION("fFxGkxF2bVo", "libScePosix", 1, "libkernel", Libraries::Net::sys_setsockopt);
    LIB_FUNCTION("RenI1lL1WFk", "libScePosix", 1, "libkernel", Libraries::Net::sys_getsockname);
    LIB_FUNCTION("KuOmgKoqCdY", "libScePosix", 1, "libkernel", Libraries::Net::sys_bind);
    LIB_FUNCTION("5jRCs2axtr4", "libScePosix", 1, "libkernel",
                 Libraries::Net::sceNetInetNtop); // TODO fix it to sys_ ...
    LIB_FUNCTION("4n51s0zEf0c", "libScePosix", 1, "libkernel",
                 Libraries::Net::sceNetInetPton); // TODO fix it to sys_ ...
    LIB_FUNCTION("XVL8So3QJUk", "libScePosix", 1, "libkernel", Libraries::Net::sys_connect);
    LIB_FUNCTION("3e+4Iv7IJ8U", "libScePosix", 1, "libkernel", Libraries::Net::sys_accept);
    LIB_FUNCTION("aNeavPDNKzA", "libScePosix", 1, "libkernel", Libraries::Net::sys_sendmsg);
    LIB_FUNCTION("pxnCmagrtao", "libScePosix", 1, "libkernel", Libraries::Net::sys_listen);

    LIB_FUNCTION("4oXYe9Xmk0Q", "libkernel", 1, "libkernel", sceKernelGetGPI);
    LIB_FUNCTION("ca7v6Cxulzs", "libkernel", 1, "libkernel", sceKernelSetGPO);
    LIB_FUNCTION("iKJMWrAumPE", "libkernel", 1, "libkernel", getargc);
    LIB_FUNCTION("FJmglmTMdr4", "libkernel", 1, "libkernel", getargv);
}

} // namespace Libraries::Kernel
