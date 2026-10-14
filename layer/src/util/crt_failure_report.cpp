#include "depthxr/crt_failure_report.h"

#include "depthxr/logger.h"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <intrin.h>

namespace depthxr {
namespace {

// ntstatus.h's codes, the ones the C runtime reported these failures with before it failed fast.
constexpr DWORD kStatusFatalAppExit = 0x40000015;              // STATUS_FATAL_APP_EXIT
constexpr DWORD kStatusInvalidCruntimeParameter = 0xC0000417;  // STATUS_INVALID_CRUNTIME_PARAMETER

std::atomic<Logger*> g_logger{nullptr};
std::atomic<DWORD> g_reporting_thread{0};

// "thread 1234", with its name if it has one (SetThreadDescription, Windows 10 1607 and later).
std::string ThreadLabel() {
    std::string label = "thread " + std::to_string(GetCurrentThreadId());
    using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    const auto get_description = reinterpret_cast<GetThreadDescriptionFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
    PWSTR description = nullptr;
    if (get_description && SUCCEEDED(get_description(GetCurrentThread(), &description)) && description) {
        char name[256]{};
        if (description[0] != L'\0' &&
            WideCharToMultiByte(CP_UTF8, 0, description, -1, name, sizeof(name), nullptr, nullptr) > 0) {
            label += " (" + std::string(name) + ")";
        }
        LocalFree(description);
    }
    return label;
}

void LogFailure(const std::string& message) {
    Logger* const logger = g_logger.load(std::memory_order_acquire);
    if (!logger) {
        return;
    }
    // Another thread may be writing a line, or the failure came from inside the logger: the line is given up
    // after a short wait, never waited for.
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (logger->TryError(message)) {
            return;
        }
        Sleep(5);
    }
}

[[noreturn]] void ReportAndEnd(const char* failure, DWORD code, unsigned int fast_fail_code) {
    const DWORD self = GetCurrentThreadId();
    DWORD reporting = 0;
    if (!g_reporting_thread.compare_exchange_strong(reporting, self)) {
        // A failure while this thread reports one ends the process at once. Another thread's report ends the
        // process itself, so this thread waits for it (a game's crash dialog may wait on the player).
        if (reporting != self) {
            Sleep(INFINITE);
        }
        __fastfail(fast_fail_code);
    }

    LogFailure("Fatal error in VectorXR on " + ThreadLabel() + ": " + failure +
               ". The game ends here; its crash report (or Windows') shows where.");

    // The report a crash in the layer makes: the game's unhandled-exception filter (its crash reporter) and
    // then Windows Error Reporting see it. Called directly rather than raised, so no handler on the stack can
    // catch it and let the game run on after a fatal error.
    CONTEXT context{};
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record{};
    record.ExceptionCode = code;
    record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
#if defined(_M_X64)
    record.ExceptionAddress = reinterpret_cast<void*>(context.Rip);
#else
    record.ExceptionAddress = _ReturnAddress();
#endif
    EXCEPTION_POINTERS pointers{&record, &context};
    if (UnhandledExceptionFilter(&pointers) != EXCEPTION_CONTINUE_SEARCH) {
        TerminateProcess(GetCurrentProcess(), code);
    }
    // A debugger is attached, or the filter passed: fail fast, as the runtime would have.
    __fastfail(fast_fail_code);
}

void OnAbort(int) {
    ReportAndEnd("abort() was called (std::terminate calls it, for example when an exception leaves a thread)",
                 kStatusFatalAppExit, FAST_FAIL_FATAL_APP_EXIT);
}

void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
    ReportAndEnd("a C runtime function was given an invalid parameter", kStatusInvalidCruntimeParameter,
                 FAST_FAIL_INVALID_ARG);
}

void OnPureCall() {
    ReportAndEnd("a pure virtual function was called", kStatusFatalAppExit, FAST_FAIL_FATAL_APP_EXIT);
}

} // namespace

void InstallCrtFailureReporting() {
    std::signal(SIGABRT, OnAbort);
    _set_invalid_parameter_handler(OnInvalidParameter);
    _set_purecall_handler(OnPureCall);
}

void SetCrtFailureLogger(Logger* logger) {
    g_logger.store(logger, std::memory_order_release);
}

} // namespace depthxr
