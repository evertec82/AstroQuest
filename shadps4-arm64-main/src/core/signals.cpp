// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstring>
#include <exception>
#include <string>
#include "common/arch.h"
#include "common/assert.h"
#include "common/crash_reporter.h"
#include "common/decoder.h"
#include "common/signal_context.h"
#include "common/types.h"
#include "core/libraries/kernel/threads/exception.h"
#include "core/signals.h"
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
#include "core/fex/fex_guest_engine.h"
#endif
#include "emulator.h"

#ifdef _WIN32
#include <windows.h>
static constexpr DWORD MS_VC_EXCEPTION = 0x406D1388;
#else
#include <csignal>
#include <pthread.h>
#include <unistd.h>
#ifdef ARCH_X86_64
#include <Zydis/Formatter.h>
#endif
#endif

#ifndef _WIN32
namespace Libraries::Kernel {
void SigactionHandler(int native_signum, siginfo_t* inf, ucontext_t* raw_context);
extern std::array<OrbisKernelExceptionHandler, 32> Handlers;
} // namespace Libraries::Kernel
#endif

namespace Core {

#if defined(_WIN32)

/// Each caller up the stack as its module and the place in it.
static void LogCallers(bool critical) {
    void* frames[32];
    const USHORT count = CaptureStackBackTrace(1, 32, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        HMODULE module = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(frames[i]), &module)) {
            GetModuleFileNameA(module, name, sizeof(name));
        }
        const char* file = std::strrchr(name, '\\');
        const auto offset =
            reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(module);
        if (critical) {
            LOG_CRITICAL(Debug, "  {} + {:#x}", file ? file + 1 : name, offset);
        } else {
            LOG_WARNING(Debug, "  {} + {:#x}", file ? file + 1 : name, offset);
        }
    }
}

/// The file name of the module at `base`, or of the one that holds `address`.
static std::string ModuleName(const void* base, const void* address = nullptr) {
    HMODULE module = static_cast<HMODULE>(const_cast<void*>(base));
    if (module == nullptr &&
        !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(address), &module)) {
        return "?";
    }
    char name[MAX_PATH] = "?";
    GetModuleFileNameA(module, name, sizeof(name));
    const char* file = std::strrchr(name, '\\');
    return file ? file + 1 : name;
}

// A C++ exception as Microsoft's compilers (and clang for Windows) throw it. Its record holds
// the object thrown, a description of the types it can be caught as, and the module those
// descriptions are offsets into.
static constexpr DWORD MSVC_CPP_EXCEPTION = 0xE06D7363;
struct CppThrowInfo {
    u32 attributes;
    s32 unwind;
    s32 forward_compat;
    s32 catchable_types;
};
struct CppCatchableType {
    u32 properties;
    s32 type;
    s32 member_displacement;
    s32 vbtable_displacement;
    s32 vbtable_index;
    s32 size;
    s32 copy;
};

/// What a thrown std::exception says, or nothing for anything else.
static std::string CppExceptionText(const EXCEPTION_RECORD& record) {
    const ULONG_PTR magic = record.NumberParameters >= 4 ? record.ExceptionInformation[0] : 0;
    if (magic < 0x19930520 || magic > 0x19930522 || record.ExceptionInformation[1] == 0 ||
        record.ExceptionInformation[2] == 0 || record.ExceptionInformation[3] == 0) {
        return {};
    }
    const auto object = record.ExceptionInformation[1];
    const auto image = record.ExceptionInformation[3];
    const auto* info = reinterpret_cast<const CppThrowInfo*>(record.ExceptionInformation[2]);
    if (info->catchable_types == 0) {
        return {};
    }
    const auto* types = reinterpret_cast<const s32*>(image + info->catchable_types);
    for (s32 i = 0; i < std::min(types[0], 16); ++i) {
        const auto* type = reinterpret_cast<const CppCatchableType*>(image + types[1 + i]);
        // A type descriptor: its table pointer, a spare pointer, then the decorated name.
        const char* name = reinterpret_cast<const char*>(image + type->type + 16);
        if (type->type != 0 && type->vbtable_displacement < 0 &&
            std::strcmp(name, ".?AVexception@std@@") == 0) {
            const auto* exception =
                reinterpret_cast<const std::exception*>(object + type->member_displacement);
            return exception->what();
        }
    }
    return {};
}

static LONG WINAPI SignalHandler(EXCEPTION_POINTERS* pExp) noexcept {
    const auto* signals = Signals::Instance();
    DWORD code = 0;
    PVOID address = nullptr;

    if (pExp != nullptr && pExp->ExceptionRecord != nullptr) {
        code = pExp->ExceptionRecord->ExceptionCode;
        address = pExp->ExceptionRecord->ExceptionAddress;
    }

    bool handled = false;
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        handled = signals->DispatchAccessViolation(
            pExp, reinterpret_cast<void*>(pExp->ExceptionRecord->ExceptionInformation[1]));
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        handled = signals->DispatchIllegalInstruction(pExp);
        break;
    case DBG_PRINTEXCEPTION_C:
    case DBG_PRINTEXCEPTION_WIDE_C:
        // Used by OutputDebugString functions.
        return EXCEPTION_CONTINUE_EXECUTION;
    case MS_VC_EXCEPTION:
        LOG_DEBUG(Debug, "Pass MS_VC_EXCEPTION at {} to handler", address);
        return EXCEPTION_EXECUTE_HANDLER;
    case MSVC_CPP_EXCEPTION: {
        // Seen here before anyone has had the chance to catch it, and most are caught: Virtual
        // Desktop's OpenXR runtime throws and catches its own while sessions come and go. It is
        // said, in case nobody does catch it, but it is no reason to stop.
        static std::atomic<u32> thrown{0};
        const u32 count = ++thrown;
        if (count <= 4 || count % 100 == 0) {
            const auto* record = pExp->ExceptionRecord;
            const std::string text = CppExceptionText(*record);
            LOG_WARNING(Debug, "A C++ exception was thrown in {} ({} so far){}{}",
                        ModuleName(record->NumberParameters >= 4
                                       ? reinterpret_cast<void*>(record->ExceptionInformation[3])
                                       : nullptr,
                                   address),
                        count, text.empty() ? "" : ": ", text);
            if (count <= 4) {
                LogCallers(false);
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    default:
        break;
    }

    if (handled) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (code == EXCEPTION_BREAKPOINT) {
        // The emulator's own come from its asserts, which have said what failed already. One
        // anywhere else stops the program just as well, and without a word.
        if (ModuleName(nullptr, address) != ModuleName(GetModuleHandleA(nullptr))) {
            LOG_CRITICAL(Debug, "A breakpoint was hit in {}", ModuleName(nullptr, address));
            LogCallers(true);
        }
        Common::Log::Flush();
    } else {
        LOG_CRITICAL(Debug, "Unhandled Exception code {:#x} at {}", code, address);
        LogCallers(true);
        Common::Singleton<Core::Emulator>::Instance()->Shutdown();
        Common::Log::Flush();
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

#else

static std::string DisassembleInstruction(void* code_address) {
    char buffer[256] = "<unable to decode>";

#ifdef ARCH_X86_64
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    const auto status =
        Common::Decoder::Instance()->decodeInstruction(instruction, operands, code_address);
    if (ZYAN_SUCCESS(status)) {
        ZydisFormatter formatter;
        ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
        ZydisFormatterFormatInstruction(&formatter, &instruction, operands,
                                        instruction.operand_count_visible, buffer, sizeof(buffer),
                                        reinterpret_cast<u64>(code_address), ZYAN_NULL);
    }
#endif

    return buffer;
}

#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
/// Reads a word of guest memory that may not be mapped. The kernel does the copy, so a bad
/// address is an error return rather than a second fault inside the fault handler.
static bool ReadGuestWord(u64 address, u64* value) {
    static int pipe_ends[2] = {-1, -1};
    if (pipe_ends[0] < 0 && pipe(pipe_ends) != 0) {
        return false;
    }
    if (write(pipe_ends[1], reinterpret_cast<const void*>(address), sizeof(*value)) !=
        static_cast<ssize_t>(sizeof(*value))) {
        return false;
    }
    return read(pipe_ends[0], value, sizeof(*value)) == static_cast<ssize_t>(sizeof(*value));
}

/// The return addresses up the guest's call stack, found through its frame pointers.
static std::string GuestCallers(u64 frame) {
    std::string callers;
    for (int depth = 0; depth < 24 && frame != 0 && (frame & 7) == 0; ++depth) {
        u64 next = 0;
        u64 return_address = 0;
        if (!ReadGuestWord(frame, &next) || !ReadGuestWord(frame + 8, &return_address)) {
            break;
        }
        callers += fmt::format(" {:#x}", return_address);
        if (next <= frame) {
            break;
        }
        frame = next;
    }
    return callers;
}
#endif

void SignalHandler(int sig, siginfo_t* info, void* raw_context) {
    Common::ReportCrash(raw_context, sig, info);
    const auto* signals = Signals::Instance();

    auto* code_address = Common::GetRip(raw_context);

    switch (sig) {
    case SIGBUS:
    case SIGSEGV: {
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
        if (sig == SIGBUS && ::Core::Fex::HandleGuestSignal(sig, info, raw_context)) {
            return;
        }
#endif
        const bool is_write = Common::IsWriteError(raw_context);
        if (!signals->DispatchAccessViolation(raw_context, info->si_addr)) {
            // If the guest has installed a custom signal handler, and the access violation didn't
            // come from HLE memory tracking, pass the signal on
            if (Libraries::Kernel::Handlers[Libraries::Kernel::NativeToOrbisSignal(sig)]) {
                Libraries::Kernel::SigactionHandler(sig, info,
                                                    reinterpret_cast<ucontext_t*>(raw_context));
                return;
            }
#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
            uint64_t guest_rip = 0;
            uint64_t guest_rax = 0;
            if (::Core::Fex::BachataQueryGuestRipSyscall(&guest_rip, &guest_rax)) {
                LOG_CRITICAL(Debug, "FEX guest state at fault: rip={:#x} rax={:#x}", guest_rip,
                             guest_rax);
            }
            // When the fault is inside an HLE function this tells which call it was: the first
            // arguments, and who made it.
            uint64_t gprs[16]{};
            if (::Core::Fex::BachataQueryGuestRegisters(gprs)) {
                const uint64_t rsp = gprs[4];
                uint64_t return_address = 0;
                if ((rsp & 7) == 0) {
                    ReadGuestWord(rsp, &return_address);
                }
                LOG_CRITICAL(Debug,
                             "FEX guest registers: rdi={:#x} rsi={:#x} rdx={:#x} rcx={:#x} "
                             "rsp={:#x} return address={:#x}",
                             gprs[7], gprs[6], gprs[2], gprs[1], rsp, return_address);
                LOG_CRITICAL(Debug, "FEX guest callers:{}", GuestCallers(gprs[5]));
            }
#endif
            UNREACHABLE_MSG("Unhandled access violation at code address {}: {} address {}",
                            fmt::ptr(code_address), is_write ? "Write to" : "Read from",
                            fmt::ptr(info->si_addr));
        }
        break;
    }
    case SIGILL:
        if (!signals->DispatchIllegalInstruction(raw_context)) {
            if (Libraries::Kernel::Handlers[Libraries::Kernel::NativeToOrbisSignal(sig)]) {
                Libraries::Kernel::SigactionHandler(sig, info,
                                                    reinterpret_cast<ucontext_t*>(raw_context));
                return;
            }
            UNREACHABLE_MSG("Unhandled illegal instruction at code address {}: {}",
                            fmt::ptr(code_address), DisassembleInstruction(code_address));
        }
        break;
    default:
        if (sig == SIGSLEEP) {
            // Sleep thread until signal is received again
            sigset_t sigset;
            sigemptyset(&sigset);
            sigaddset(&sigset, SIGSLEEP);
            sigwait(&sigset, &sig);
        }
        break;
    }
}

#endif

SignalDispatch::SignalDispatch() {
    Common::InitCrashReporter();
#if defined(_WIN32)
    ASSERT_MSG(handle = AddVectoredExceptionHandler(0, SignalHandler),
               "Failed to register exception handler.");
#else
    struct sigaction action{};
    action.sa_sigaction = SignalHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);

    ASSERT_MSG(sigaction(SIGSEGV, &action, nullptr) == 0 &&
                   sigaction(SIGBUS, &action, nullptr) == 0,
               "Failed to register access violation signal handler.");
    ASSERT_MSG(sigaction(SIGILL, &action, nullptr) == 0,
               "Failed to register illegal instruction signal handler.");
    ASSERT_MSG(sigaction(SIGSLEEP, &action, nullptr) == 0,
               "Failed to register sleep signal handler.");
#endif
}

SignalDispatch::~SignalDispatch() {
#if defined(_WIN32)
    ASSERT_MSG(RemoveVectoredExceptionHandler(handle), "Failed to remove exception handler.");
#else
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    action.sa_flags = 0;
    sigemptyset(&action.sa_mask);

    ASSERT_MSG(sigaction(SIGSEGV, &action, nullptr) == 0 &&
                   sigaction(SIGBUS, &action, nullptr) == 0,
               "Failed to remove access violation signal handler.");
    ASSERT_MSG(sigaction(SIGILL, &action, nullptr) == 0,
               "Failed to remove illegal instruction signal handler.");
#endif
}

bool SignalDispatch::DispatchAccessViolation(void* context, void* fault_address) const {
    for (const auto& [handler, _] : access_violation_handlers) {
        if (handler(context, fault_address)) {
            return true;
        }
    }
    return false;
}

bool SignalDispatch::DispatchIllegalInstruction(void* context) const {
    for (const auto& [handler, _] : illegal_instruction_handlers) {
        if (handler(context)) {
            return true;
        }
    }
    return false;
}

} // namespace Core
