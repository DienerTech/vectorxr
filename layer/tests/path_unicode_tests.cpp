// A profile folder outside the ANSI code page (a Cyrillic user name on an
// English-locale system): the layer finds the same folders as the app and
// never throws converting a path to text.
#include "depthxr/config_parser.h"
#include "depthxr/config_path.h"
#include "depthxr/external_status.h"
#include "depthxr/logger.h"
#include "depthxr/process_info.h"
#include "depthxr/runtime_pacing.h"
#include "depthxr/runtime_relay.h"
#include "depthxr/seen_apps.h"
#include "depthxr/turbo_metrics.h"
#include "depthxr/turbo_recovery.h"
#include "depthxr/turbo_trace.h"

#include <Windows.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

void Expect(bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

bool Contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

std::string ReadAll(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

} // namespace

int main() {
    if (GetACP() == CP_UTF8) {
        std::cout << "The ANSI code page is UTF-8: narrow paths would pass these checks too.\n";
    }
    // A Cyrillic name and a Polish letter (U+0410 U+043D U+0434 U+0440 U+0435
    // U+0439, U+0141): neither is in code page 1252, which gives "?" for the
    // first and "L" for the second.
    const std::wstring profile_name{0x0410, 0x043D, 0x0434, 0x0440, 0x0435, 0x0439, L' ', 0x0141};
    const std::filesystem::path test_root =
        std::filesystem::temp_directory_path() /
        (L"vectorxr-path-test-" + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::filesystem::path local_app_data = test_root / profile_name;
    std::filesystem::create_directories(local_app_data);
    _wputenv_s(L"LOCALAPPDATA", local_app_data.c_str());
    for (const wchar_t* name : {L"VECTORXR_CONFIG_PATH", L"VECTORXR_SEEN_APPS_PATH", L"VECTORXR_LOG_PATH",
                                L"VECTORXR_RUNTIME_PACING_PATH", L"VECTORXR_RUNTIME_RELAY_PATH", L"VECTORXR_EXTERNAL_STATUS_PATH",
                                L"VECTORXR_TURBO_METRICS_PATH"}) {
        _wputenv_s(name, L"");
    }
    const std::filesystem::path root = local_app_data / "VectorXR";

    // The app's folders.
    Expect(depthxr::ResolveConfigPath() == root / "config" / "settings.json", "config path");
    Expect(depthxr::ResolveSeenAppsPath() == root / "config" / "seen-apps.json", "seen-apps path");
    Expect(depthxr::ResolveLogPath() == root / "logs" / "vectorxr.log", "log path");
    Expect(depthxr::ResolveRuntimePacingPath() == root / "config" / "runtime-pacing.json", "runtime-pacing path");
    Expect(depthxr::ResolveTurboMetricsPath() == root / "config" / "turbo-metrics.json", "turbo-metrics path");
    Expect(depthxr::ResolveRuntimeRelayRoot() == root / "runtime", "runtime relay folder");
    Expect(depthxr::ResolveExternalStatusPath() == root / "api" / "status.json", "public status file");

    // Log text is UTF-8, as the app reads it; an unpaired surrogate (a valid
    // NTFS name) is replaced rather than thrown on.
    Expect(depthxr::PathToUtf8(local_app_data.filename()) ==
               "\xD0\x90\xD0\xBD\xD0\xB4\xD1\x80\xD0\xB5\xD0\xB9 \xC5\x81",
           "path text is UTF-8");
    Expect(depthxr::PathToUtf8(std::filesystem::path(std::wstring{L'a', static_cast<wchar_t>(0xD800)})).size() > 1,
           "an unpaired surrogate is replaced");
    Expect(depthxr::GetCurrentExecutableName() == "depthxr_path_unicode_tests.exe", "executable name");

    // The log, with a file named outside the code page in its folder (pruning
    // reads every name there).
    std::filesystem::create_directories(root / "logs");
    std::ofstream(root / "logs" / (L"vectorxr-" + profile_name + L".log")) << "stray\n";
    {
        depthxr::Logger logger;
        logger.Initialize(depthxr::ResolveLogPath());
        logger.SetRetentionFiles(1);
        logger.SetLevel(depthxr::LogLevel::Debug);
        logger.Error("Active log file: " + depthxr::PathToUtf8(logger.ActiveLogPath()));
        Expect(logger.ActiveLogPath().parent_path() == root / "logs", "the log is in the app's log folder");
        Expect(Contains(ReadAll(logger.ActiveLogPath()), "Active log file: " + depthxr::PathToUtf8(root)),
               "the log names its folder in UTF-8");

        depthxr::TurboTimingTrace trace;
        trace.Start(logger);
        trace.Stop();
        logger.Error("trace stopped");
        Expect(Contains(ReadAll(logger.ActiveLogPath()),
                        "Turbo trace v3: continuous recent history in " +
                            depthxr::PathToUtf8(depthxr::TurboTimingTrace::RecorderPath(logger.ActiveLogPath(), 0))),
               "the turbo trace names its recorder in UTF-8");
    }
    // A log folder that cannot be made leaves the logger without a file.
    {
        depthxr::Logger logger;
        logger.Initialize(local_app_data / "bad?folder" / "vectorxr.log");
        logger.Error("dropped");
    }

    // A missing config file is reported with its path.
    const depthxr::ParseResult parsed = depthxr::LoadConfigFromFile(depthxr::ResolveConfigPath());
    Expect(!parsed.ok && Contains(parsed.error, depthxr::PathToUtf8(root)), "missing config error: " + parsed.error);

    // The layer's own files, each written through a temporary next to it.
    std::string error;
    Expect(depthxr::RecordSeenApp("Game.exe", &error), "seen apps: " + error);
    Expect(std::filesystem::exists(root / "config" / "seen-apps.json"), "seen-apps file");

    depthxr::TurboMetricsSession session;
    session.session_id = "Game.exe-1";
    session.app_name = "Game.exe";
    Expect(depthxr::RecordTurboMetricsSession(depthxr::ResolveTurboMetricsPath(), session, &error),
           "turbo metrics: " + error);
    Expect(std::filesystem::exists(root / "config" / "turbo-metrics.json"), "turbo-metrics file");

    depthxr::RuntimePacingObservation observation;
    observation.test_started_at = 1;
    observation.runtime_name = "Test Runtime";
    Expect(depthxr::RecordRuntimePacingObservation(depthxr::ResolveRuntimePacingPath(), observation, &error),
           "runtime pacing: " + error);
    Expect(std::filesystem::exists(root / "config" / "runtime-pacing.json"), "runtime-pacing file");

    const std::filesystem::path status_path =
        depthxr::RuntimeStatusPath(depthxr::ResolveRuntimeRelayRoot(), "1-2-3");
    Expect(depthxr::WriteRuntimeStatus(status_path, depthxr::RuntimeStatusDocument{}, &error),
           "runtime status: " + error);
    Expect(std::filesystem::exists(root / "runtime" / "status" / "1-2-3.json"), "runtime status file");
    depthxr::ExternalStatusDocument external;
    external.process_id = 1;
    Expect(depthxr::WriteExternalStatus(depthxr::ResolveExternalStatusPath(), external, &error), "public status: " + error);
    Expect(std::filesystem::exists(root / "api" / "status.json"), "public status file written");

    depthxr::TurboRecoveryRecord identity;
    identity.fingerprint = "test";
    identity.application = "Game.exe";
    identity.process_id = 42;
    identity.process_created = 1;
    depthxr::TurboRecoveryGuard guard;
    guard.Configure(depthxr::ResolveRuntimeRelayRoot() / "turbo-recovery", identity);
    Expect(guard.Prepare(true, false, "async", [](std::uint32_t, std::uint64_t) { return false; }),
           "turbo recovery arms");
    Expect(std::filesystem::exists(root / "runtime" / "turbo-recovery"), "turbo recovery record");
    guard.Fail("test failure");
    guard.Complete();
    Expect(std::filesystem::exists(root / "runtime" / "turbo-recovery" / "faults"), "turbo recovery fault");

    std::error_code ignored;
    std::filesystem::remove_all(test_root, ignored);
    std::cout << "path_unicode_tests passed\n";
    return 0;
}
