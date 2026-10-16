// What DllMain sees as a process ends. Each case runs a child that loads
// depthxr_process_exit_probe.dll (the layer's DllMain), arms an exit file and
// ends one way: a normal exit must remove the file before the CRT destroys the
// DLL's statics, while TerminateProcess (a crash, Task Manager) and an unload
// must leave it. So must a fatal error in the layer, even when the game's crash
// reporter then ends the process with ExitProcess. CPU only.
#include <Windows.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

void Expect(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

std::filesystem::path ModuleDirectory() {
    wchar_t buffer[MAX_PATH * 4]{};
    GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    return std::filesystem::path(buffer).parent_path();
}

// A game's crash reporter that ends the process normally once its report is
// written (or the player closed its dialog).
LONG WINAPI ExitingCrashReporter(EXCEPTION_POINTERS*) {
    ExitProcess(0);
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream stream(path);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

int Child(const std::wstring& how, const std::filesystem::path& file, const std::filesystem::path& sentinel) {
    HMODULE probe = LoadLibraryW((ModuleDirectory() / L"depthxr_process_exit_probe.dll").c_str());
    if (!probe) return 10;
    if (how == L"log") {
        using Write = void (*)(const wchar_t*);
        const auto write = reinterpret_cast<Write>(GetProcAddress(probe, "WriteExitLog"));
        if (!write) return 13;
        write(file.c_str());
        ExitProcess(0);
    }
    using Arm = int (*)(const wchar_t*, const wchar_t*);
    const auto arm = reinterpret_cast<Arm>(GetProcAddress(probe, "ArmProcessExitFile"));
    if (!arm || !arm(file.c_str(), sentinel.c_str())) return 11;
    if (how == L"exit") ExitProcess(0);
    if (how == L"terminate") TerminateProcess(GetCurrentProcess(), 0);
    if (how == L"unload") {
        FreeLibrary(probe);
        ExitProcess(0);
    }
    if (how == L"fatal") {
        using Fail = void (*)();
        const auto fail = reinterpret_cast<Fail>(GetProcAddress(probe, "FailFatally"));
        if (!fail) return 14;
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        SetUnhandledExceptionFilter(ExitingCrashReporter);
        fail();
        return 15;
    }
    return 12;
}

DWORD RunChild(const wchar_t* how, const std::filesystem::path& file, const std::filesystem::path& sentinel) {
    wchar_t self[MAX_PATH * 4]{};
    GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    std::wstring command = L"\"" + std::wstring(self) + L"\" child " + how + L" \"" + file.wstring() + L"\" \"" +
                           sentinel.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    Expect(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process) != FALSE,
           "Could not start the child process");
    Expect(WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0, "The child process did not end");
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return code;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 5 && std::wstring(argv[1]) == L"child") return Child(argv[2], argv[3], argv[4]);

    const auto root = std::filesystem::temp_directory_path() /
        ("vectorxr-process-exit-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto run = [&](const wchar_t* how) {
        const auto file = root / (std::wstring(how) + L".json");
        const auto sentinel = root / (std::wstring(how) + L".sentinel");
        std::ofstream(file) << "{}";
        Expect(RunChild(how, file, sentinel) == 0, "The child process failed");
        return std::make_pair(std::filesystem::exists(file), ReadText(sentinel));
    };

    const auto [exit_kept, exit_sentinel] = run(L"exit");
    Expect(!exit_kept, "A normal exit must remove an armed exit file");
    Expect(exit_sentinel == "exiting", "Static destructors at a normal exit must see ProcessExiting()");

    const auto [terminate_kept, terminate_sentinel] = run(L"terminate");
    Expect(terminate_kept, "TerminateProcess must leave the exit file as evidence");
    Expect(terminate_sentinel.empty(), "TerminateProcess runs no static destructors");

    const auto [unload_kept, unload_sentinel] = run(L"unload");
    Expect(unload_kept, "An unload is not a process exit");
    Expect(unload_sentinel == "unloading", "Static destructors at an unload must not see ProcessExiting()");

    // The layer's fatal error went to the game's crash reporter, which then
    // exited: a crash, so the Turbo recovery marker must stay for next launch.
    const auto [fatal_kept, fatal_sentinel] = run(L"fatal");
    Expect(fatal_kept, "A fatal layer error must leave the exit file, however the game then exits");
    Expect(fatal_sentinel == "exiting", "The crash reporter's ExitProcess is still a process exit");

    // The layer's logger outlives a process exit: its pending "repeated N
    // times" line and buffered lines must still reach the file.
    const auto log_directory = root / L"log";
    Expect(RunChild(L"log", log_directory / L"exit.log", root / L"log.sentinel") == 0, "The logging child failed");
    std::string log_text;
    for (const auto& entry : std::filesystem::directory_iterator(log_directory)) log_text += ReadText(entry.path());
    Expect(log_text.find("first line") != std::string::npos && log_text.find("repeated line") != std::string::npos,
           "A normal exit lost the log's buffered lines");
    Expect(log_text.find("message repeated 2 additional times") != std::string::npos,
           "A normal exit lost the log's pending repeated-line summary");

    std::filesystem::remove_all(root);
    std::cout << "process_exit_tests passed\n";
    return 0;
}
