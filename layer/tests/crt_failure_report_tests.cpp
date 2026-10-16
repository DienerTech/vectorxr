// The layer carries its own C runtime (/MT), so a game's crash handlers never see the layer's fatal runtime
// errors, which fail fast by default. crt_failure_report hands them on. Each case runs in a child process that
// installs it as the layer does, sets an unhandled-exception filter as a game's crash reporter does, and fails on
// purpose: the filter must see the failure with its status code, and the log must name it. A child run with
// --no-reporting shows the default: it fails fast past the filter (exit code 0xC0000409). In the
// concurrent-abort case a second thread aborts while the first report is with the filter: it must wait for that
// report, not fail fast under it.
#include "depthxr/crt_failure_report.h"
#include "depthxr/logger.h"

#include <Windows.h>
#if defined(_DEBUG)
#include <crtdbg.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

// What a pure virtual function's slot in a vtable calls.
extern "C" int __cdecl _purecall();

namespace {

constexpr DWORD kStatusFatalAppExit = 0x40000015;
constexpr DWORD kStatusInvalidCruntimeParameter = 0xC0000417;
// The child's crash reporter ends it with one of these.
constexpr DWORD kReportedExitCode = 0x0EC0;
constexpr DWORD kWrongReportExitCode = 0x0EC1;

struct Case {
    const wchar_t* name;
    DWORD code;
    const char* logged;
};

constexpr Case kCases[] = {
    {L"abort", kStatusFatalAppExit, "abort() was called"},
    {L"thread-exception", kStatusFatalAppExit, "abort() was called"},
    {L"invalid-parameter", kStatusInvalidCruntimeParameter, "was given an invalid parameter"},
    {L"pure-call", kStatusFatalAppExit, "a pure virtual function was called"},
    {L"concurrent-abort", kStatusFatalAppExit, "abort() was called"},
};

DWORD g_expected_code = 0;
bool g_abort_during_report = false;

void Expect(bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

std::string Narrow(std::wstring_view text) {
    std::string narrow;
    for (const wchar_t character : text) {
        narrow += static_cast<char>(character);
    }
    return narrow;
}

std::string Hex(DWORD value) {
    char text[16]{};
    std::snprintf(text, sizeof(text), "0x%08lX", value);
    return text;
}

LONG WINAPI GameCrashReporter(EXCEPTION_POINTERS* pointers) {
    bool expected = pointers && pointers->ExceptionRecord && pointers->ContextRecord &&
                    pointers->ExceptionRecord->ExceptionCode == g_expected_code &&
                    (pointers->ExceptionRecord->ExceptionFlags & EXCEPTION_NONCONTINUABLE) != 0;
    if (g_abort_during_report) {
        // Another layer thread aborts while the game writes its report. Failing fast would end the process here
        // with 0xC0000409; it must wait instead, and never return.
        const HANDLE aborting = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::thread second([aborting] {
            SetEvent(aborting);
            std::abort();
        });
        expected = expected && WaitForSingleObject(aborting, 5000) == WAIT_OBJECT_0 &&
                   WaitForSingleObject(second.native_handle(), 1000) == WAIT_TIMEOUT;
        second.detach();
    }
    TerminateProcess(GetCurrentProcess(), expected ? kReportedExitCode : kWrongReportExitCode);
    return EXCEPTION_EXECUTE_HANDLER;
}

int RunChild(std::wstring_view name, const std::filesystem::path& log_directory, bool report) {
    // No Windows Error Reporting dialog if a failure gets past the filter.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#if defined(_DEBUG)
    _set_abort_behavior(0, _WRITE_ABORT_MSG);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG);
#endif
    depthxr::Logger logger;
    logger.Initialize(log_directory / "vectorxr.log");
    if (report) {
        depthxr::SetCrtFailureLogger(&logger);
        depthxr::InstallCrtFailureReporting();
    }
    SetUnhandledExceptionFilter(GameCrashReporter);

    for (const Case& test_case : kCases) {
        if (name == test_case.name) {
            g_expected_code = test_case.code;
        }
    }
    if (name == L"abort") {
        std::abort();
    } else if (name == L"concurrent-abort") {
        g_abort_during_report = true;
        std::abort();
    } else if (name == L"thread-exception") {
        std::thread([] { throw std::runtime_error("an exception leaves a layer thread"); }).join();
    } else if (name == L"invalid-parameter") {
        char* volatile destination = nullptr;
        strcpy_s(destination, 4, "x");
    } else if (name == L"pure-call") {
        _purecall();
    }
    return 2; // Nothing ended the process.
}

std::string ReadLogs(const std::filesystem::path& directory) {
    std::string text;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (entry.path().extension() == ".log") {
            std::ifstream stream(entry.path(), std::ios::binary);
            text.append(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        }
    }
    return text;
}

void RunCase(const std::filesystem::path& exe, const std::filesystem::path& root, const Case& test_case) {
    const std::string label = Narrow(test_case.name) + ": ";
    const std::filesystem::path directory = root / test_case.name;
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    Expect(!error, label + "the log folder is created");

    std::wstring command =
        L"\"" + exe.wstring() + L"\" --child " + test_case.name + L" \"" + directory.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    Expect(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                          &process) != FALSE,
           label + "the child starts (error " + std::to_string(GetLastError()) + ")");
    const DWORD waited = WaitForSingleObject(process.hProcess, 30000);
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
    }
    DWORD exit_code = 0;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    Expect(waited == WAIT_OBJECT_0, label + "the child ends instead of hanging");
    Expect(exit_code != kWrongReportExitCode,
           label + "the crash reporter sees status " + Hex(test_case.code) + ", noncontinuable");
    Expect(exit_code == kReportedExitCode,
           label + "the crash reporter sees the failure (the child exited with " + Hex(exit_code) + ")");
    const std::string log = ReadLogs(directory);
    Expect(log.find("[error] Fatal error in VectorXR on thread ") != std::string::npos,
           label + "the log names the failure and its thread");
    Expect(log.find(test_case.logged) != std::string::npos, label + "the log says \"" + test_case.logged + "\"");
    std::filesystem::remove_all(directory, error);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 4 && std::wstring_view(argv[1]) == L"--child") {
        return RunChild(argv[2], argv[3], !(argc >= 5 && std::wstring_view(argv[4]) == L"--no-reporting"));
    }

    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const std::filesystem::path exe(exe_path);
    const std::filesystem::path root = exe.parent_path() / "crt_failure_report_tests";
    for (const Case& test_case : kCases) {
        RunCase(exe, root, test_case);
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);

    std::cout << "CRT failure report tests passed: abort, an exception leaving a thread, an invalid parameter and a "
                 "pure virtual call each reach the crash reporter and the log, and an abort during a report waits "
                 "for it\n";
    return 0;
}
