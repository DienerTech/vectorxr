#include "depthxr/runtime_relay.h"
#include "depthxr/atomic_file.h"
#include "depthxr/turbo_metrics.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <thread>

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

#if defined(_WIN32)
    // Antivirus or indexing holding a file open without delete sharing makes
    // a replace fail for a moment; the shared replace waits it out.
    const auto busy = root / "busy.json";
    Touch(busy, "old");
    for (const bool hold_source : {false, true}) {
        const auto temporary = depthxr::UniqueTemporaryPath(busy);
        Touch(temporary, "new");
        HANDLE held = CreateFileW((hold_source ? temporary : busy).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Expect(held != INVALID_HANDLE_VALUE, "Could not hold the file open");
        std::thread release([held] {
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            CloseHandle(held);
        });
        std::uint32_t error_code = 0;
        const bool replaced = depthxr::ReplaceFileAtomically(temporary, busy, &error_code);
        release.join();
        Expect(replaced, "A briefly held file should be replaced after a retry");
        std::ifstream stream(busy);
        std::string content;
        stream >> content;
        Expect(content == "new", "The replaced file should hold the new content");
        Touch(busy, "old");
    }

    // Turbo metrics went through delete-then-rename with no retry: a scan of
    // the file failed the flush, and a scan of its temporary lost the history.
    const auto metrics = root / "turbo-metrics.json";
    depthxr::TurboMetricsSession first;
    first.session_id = "StarCitizen.exe-1";
    first.app_name = "StarCitizen.exe";
    first.started_unix_seconds = 1;
    Expect(depthxr::RecordTurboMetricsSession(metrics, first, nullptr), "The first metrics session should be written");
    HANDLE scanner = CreateFileW(metrics.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    Expect(scanner != INVALID_HANDLE_VALUE, "Could not open the metrics file as a scanner would");
    std::thread scan([scanner] {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        CloseHandle(scanner);
    });
    auto second = first;
    second.session_id = "StarCitizen.exe-2";
    second.started_unix_seconds = 2;
    const bool recorded = depthxr::RecordTurboMetricsSession(metrics, second, nullptr);
    scan.join();
    const auto sessions = depthxr::ReadTurboMetricsSessions(metrics);
    Expect(recorded && sessions.size() == 2, "A briefly scanned turbo-metrics.json should still take the new session");
#endif

    std::filesystem::remove_all(root);
    std::cout << "runtime_relay_tests passed\n";
    return 0;
}
