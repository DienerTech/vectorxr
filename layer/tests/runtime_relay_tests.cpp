#include "depthxr/runtime_relay.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace {

void Expect(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void Touch(const std::filesystem::path& path, const std::string& content = "{}") {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << content;
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("vectorxr-relay-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    // Sessions of process 1234 ended without teardown (one status file is
    // zero-filled, as after a hard shutdown); process 5678 is still running.
    Touch(root / "status" / "1234-1791949116543-1.json", std::string(64, '\0'));
    Touch(root / "status" / "1234-1791949116543-1.json.tmp.5.6");
    Touch(root / "control" / "1234-1791949116543-1.json");
    Touch(root / "status" / "1234-1791949999999-2.json");
    Touch(root / "status" / "5678-1791950000000-1.json");
    Touch(root / "control" / "5678-1791950000000-1.json");
    Touch(root / "status" / "notes.txt");
    Touch(root / "status" / "app-1791949116543-1.json");
    Touch(root / "status" / "99999999999-1-1.json"); // Not a process id.

    std::set<std::uint64_t> asked;
    const std::size_t removed = depthxr::PruneRuntimeRelayFiles(root, [&](std::uint32_t pid, std::uint64_t started) {
        asked.insert(started);
        return pid == 5678;
    });
    Expect(removed == 4, "Every file of an ended session should be removed");
    Expect(!std::filesystem::exists(root / "status" / "1234-1791949116543-1.json"), "A zero-filled status file should be removed");
    Expect(!std::filesystem::exists(root / "status" / "1234-1791949116543-1.json.tmp.5.6"), "A leftover temporary should be removed");
    Expect(!std::filesystem::exists(root / "control" / "1234-1791949116543-1.json"), "An ended session's control file should be removed");
    Expect(std::filesystem::exists(root / "status" / "5678-1791950000000-1.json") &&
               std::filesystem::exists(root / "control" / "5678-1791950000000-1.json"),
           "A running session's files must stay");
    Expect(std::filesystem::exists(root / "status" / "notes.txt") &&
               std::filesystem::exists(root / "status" / "app-1791949116543-1.json") &&
               std::filesystem::exists(root / "status" / "99999999999-1-1.json"),
           "Files that are not session files must stay");
    Expect(asked.count(1791949116543) == 1 && asked.count(1791949999999) == 1,
           "The session start time should reach the liveness check");
    Expect(depthxr::PruneRuntimeRelayFiles(root / "missing") == 0, "A missing relay folder is not an error");

#if defined(_WIN32)
    const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    Expect(depthxr::RuntimeRelaySessionAlive(GetCurrentProcessId(), now), "This process is alive");
    Expect(!depthxr::RuntimeRelaySessionAlive(GetCurrentProcessId(), 1000),
           "A process created after the session started reuses the session's process id");

    wchar_t command[] = L"cmd.exe /c exit 0";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, command, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        WaitForSingleObject(process.hProcess, 10000);
        Expect(!depthxr::RuntimeRelaySessionAlive(process.dwProcessId, now),
               "An exited process (handle still open) is not alive");
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
#endif

    std::filesystem::remove_all(root);
    std::cout << "runtime_relay_tests passed\n";
    return 0;
}
