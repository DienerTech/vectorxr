#include "depthxr/turbo_recovery.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

void Expect(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}

// A guard that would not arm says why (a recovery path too long for the
// file system failed CI's "First run should arm" with no reason).
void Expect(bool value, const char* message, const std::string& reason) {
    if (!value) {
        std::cerr << message << " (" << (reason.empty() ? "no reason given" : reason) << ")\n";
        std::exit(1);
    }
}

int main() {
    // Short, so a long %TEMP% leaves room for the records' own names.
    const auto root = std::filesystem::temp_directory_path() / ("vxr-recovery-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 1'000'000'000));
    const auto dead = [](std::uint32_t, std::uint64_t) { return false; };
    const auto alive = [](std::uint32_t, std::uint64_t) { return true; };
    depthxr::TurboRecoveryRecord identity;
    identity.fingerprint = "dcs:steamvr:d3d11";
    identity.application = "DCS.exe";
    identity.runtime = "SteamVR";
    identity.process_id = 42;
    identity.process_created = 123456789012345678;
    {
        depthxr::TurboRecoveryGuard first;
        first.Configure(root, identity);
        Expect(first.Prepare(true, false, "async", dead), "First run should arm", first.Reason());
        // Intentionally no Complete: simulate a crash without destructor cleanup.
    }
    depthxr::TurboRecoveryGuard concurrent;
    concurrent.Configure(root, identity);
    Expect(concurrent.Prepare(true, false, "async", alive), "A live process is not an interrupted run", concurrent.Reason());
    concurrent.Complete();

    depthxr::TurboRecoveryGuard next;
    next.Configure(root, identity);
    Expect(!next.Prepare(true, false, "async", dead) && next.Blocked(), "Dead process should block the next run");
    Expect(next.Reason().find("ended without a clean shutdown") != std::string::npos &&
           next.Reason().find("skips OpenXR cleanup") != std::string::npos,
           "A dead process's block should not claim it crashed: a game's own quit can leave the marker too");
    next.Complete();
    Expect(!next.Prepare(true, false, "async", dead), "A clean Turbo-off run must preserve the interruption");
    Expect(next.Prepare(false, false, "async", dead), "Opt-out must bypass recovery", next.Reason());
    Expect(!next.Prepare(true, false, "async", dead), "Re-enabling recovery must restore the unresolved interruption");

    auto other_identity = identity;
    other_identity.fingerprint = "other:steamvr:d3d11";
    depthxr::TurboRecoveryGuard other;
    other.Configure(root, other_identity);
    Expect(other.Prepare(true, false, "async", dead), "An interruption must not block a different app/setup", other.Reason());
    other.Complete();

    Expect(next.Prepare(true, true, "sequenced", dead), "Explicit retry should supersede the previous interruption", next.Reason());
    next.Complete();
    depthxr::TurboRecoveryGuard clean;
    clean.Configure(root, identity);
    Expect(clean.Prepare(true, false, "async", dead), "A clean retry should clear recovery history", clean.Reason());
    clean.Fail("Repeated runtime submission errors");
    clean.Complete();
    depthxr::TurboRecoveryGuard failed;
    failed.Configure(root, identity);
    Expect(!failed.Prepare(true, false, "async", alive), "Runtime failures survive clean shutdown and do not depend on process death");
    Expect(failed.Reason().find("runtime errors") != std::string::npos, "A runtime failure's block should name the runtime errors");
    Expect(failed.Prepare(true, true, "async", dead), "Failure recovery can be explicitly retried");
    failed.Prepare(false, false, "async", dead);
    depthxr::TurboRecoveryGuard bypassed;
    bypassed.Configure(root, identity);
    Expect(bypassed.Prepare(true, false, "async", dead), "Disabling the watch during a run removes its own marker", bypassed.Reason());
    bypassed.Complete();

    Expect(std::filesystem::exists(root / "faults"), "Retries must retain fault history");
    const auto fault_count = std::distance(std::filesystem::directory_iterator(root / "faults"), std::filesystem::directory_iterator());
    Expect(fault_count >= 2, "Crash and runtime failure must both be archived");
    depthxr::TurboRecoveryGuard ui_clear;
    ui_clear.Configure(root, identity);
    Expect(ui_clear.Prepare(true, false, "async", dead), "UI clear test should start clean", ui_clear.Reason());
    ui_clear.Fail("UI clear test failure");
    // Model the UI archive-and-remove action. Faults already archived by Fail.
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.path().extension() == ".json") std::filesystem::remove(entry.path());
    }
    Expect(ui_clear.ConsumeClearRequest(), "A live suspended session should observe UI clearing its failed marker");
    Expect(ui_clear.Prepare(true, true, "async", dead), "UI clear should allow the session to re-arm", ui_clear.Reason());
    ui_clear.Complete();
    Expect(std::distance(std::filesystem::directory_iterator(root / "faults"), std::filesystem::directory_iterator()) > fault_count,
           "UI clear must retain the failure details");
    depthxr::TurboRecoveryGuard opted_out;
    opted_out.Configure(root, identity);
    opted_out.Prepare(false, false, "sequenced", dead);
    opted_out.Fail("Fault with safety bypassed");
    depthxr::TurboRecoveryGuard after_opt_out;
    after_opt_out.Configure(root, identity);
    Expect(after_opt_out.Prepare(true, false, "async", dead), "Opt-out diagnostics must not create a block", after_opt_out.Reason());
    after_opt_out.Complete();

    const auto clear_root = root / "clear-logs";
    depthxr::TurboRecoveryGuard logged;
    logged.Configure(clear_root, identity);
    Expect(logged.Prepare(true, false, "async", dead), "Clear log scenario should arm", logged.Reason());
    logged.Fail("Fault to clear");
    // Model the UI's per-event clear markers, preserving recovery records.
    std::filesystem::create_directories(clear_root / "faults-cleared");
    for (const auto& entry : std::filesystem::directory_iterator(clear_root / "faults")) {
        auto marker = clear_root / "faults-cleared" / entry.path().filename();
        marker.replace_extension("txt");
        std::ofstream(marker) << "";
        std::filesystem::remove(entry.path());
    }
    depthxr::TurboRecoveryGuard still_blocked;
    still_blocked.Configure(clear_root, identity);
    Expect(!still_blocked.Prepare(true, false, "async", dead), "Clearing fault logs must preserve protection");
    Expect(still_blocked.Prepare(true, true, "async", dead), "Cleared fault can still be explicitly retried", still_blocked.Reason());
    Expect(std::filesystem::is_empty(clear_root / "faults"), "Retry must not resurrect cleared fault history");
    still_blocked.Fail("New fault after clearing");
    Expect(!std::filesystem::is_empty(clear_root / "faults"), "New faults must still be archived");

    // Star Citizen and MSFS exit without OpenXR teardown. A normal process
    // exit (DllMain's DLL_PROCESS_DETACH) is not an interrupted run, but a
    // crash or forced termination still is, and a failure survives any exit.
    const auto exit_root = root / "process-exit";
    {
        depthxr::TurboRecoveryGuard exited;
        exited.Configure(exit_root, identity);
        Expect(exited.Prepare(true, false, "async", dead), "Exit scenario should arm");
        depthxr::RemoveProcessExitFiles(); // What DllMain does at a normal exit.
    }
    {
        depthxr::TurboRecoveryGuard after_exit;
        after_exit.Configure(exit_root, identity);
        Expect(after_exit.Prepare(true, false, "async", dead), "A normal exit without OpenXR teardown must not block Turbo");
        // No exit and no Complete: a crash or TerminateProcess.
    }
    {
        depthxr::TurboRecoveryGuard after_crash;
        after_crash.Configure(exit_root, identity);
        Expect(!after_crash.Prepare(true, false, "async", dead) && after_crash.Blocked(), "A crash must still block the next run");
        Expect(after_crash.Prepare(true, true, "async", dead), "A crash block can be retried");
        after_crash.Fail("Runtime failure before a normal exit");
        depthxr::RemoveProcessExitFiles();
    }
    {
        depthxr::TurboRecoveryGuard after_failure;
        after_failure.Configure(exit_root, identity);
        Expect(!after_failure.Prepare(true, false, "async", alive), "A normal exit must keep a runtime failure's marker");
        Expect(after_failure.Prepare(true, true, "async", dead), "The failure can be retried");
        after_failure.Prepare(false, false, "async", dead); // Watch turned off mid-run.
        depthxr::TurboRecoveryGuard sibling;
        sibling.Configure(exit_root, identity);
        Expect(sibling.Prepare(true, false, "async", alive), "A second guard should arm");
        sibling.Complete();
        depthxr::RemoveProcessExitFiles();
        Expect(std::none_of(std::filesystem::directory_iterator(exit_root), std::filesystem::directory_iterator(),
                            [](const auto& entry) { return entry.path().extension() == ".json"; }),
               "Disarmed guards must leave nothing behind");
    }
    {
        depthxr::TurboRecoveryGuard after_disarm;
        after_disarm.Configure(exit_root, identity);
        Expect(after_disarm.Prepare(true, false, "async", dead), "Disarmed guards must not block the next run");
        const auto own = std::find_if(std::filesystem::directory_iterator(exit_root), std::filesystem::directory_iterator(),
                                      [](const auto& entry) { return entry.path().extension() == ".json"; });
        Expect(own != std::filesystem::directory_iterator(), "An armed guard keeps its marker until the process exits");
        depthxr::RemoveProcessExitFiles();
        depthxr::RemoveProcessExitFiles(); // DllMain runs once; the table must not care.
        after_disarm.Complete();
    }

    // A fault Turbo recovered from is history only. The live marker is what
    // the next run reads after a crash, so it must stay exactly as armed.
    const auto read_file = [](const std::filesystem::path& path) {
        std::ifstream stream(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    };
    const auto find_marker = [](const std::filesystem::path& directory) {
        std::filesystem::path marker;
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (entry.path().extension() == ".json") marker = entry.path();
        }
        return marker;
    };
    const auto recovered_root = root / "recovered";
    {
        depthxr::TurboRecoveryGuard recovering;
        recovering.Configure(recovered_root, identity);
        Expect(recovering.Prepare(true, false, "async", dead), "Recovered scenario should arm", recovering.Reason());
        const auto marker = find_marker(recovered_root);
        Expect(!marker.empty(), "An armed guard has a marker");
        const std::string before = read_file(marker);
        Expect(before.find("\"state\": \"armed\"") != std::string::npos &&
               before.find("\"processId\": 42") != std::string::npos,
               "The marker should be armed for this process");
        recovering.NoteRecovered("Recovered from a runtime frame-call failure");
        Expect(read_file(marker) == before, "Noting a recovered fault must not touch the live marker");
        Expect(recovering.Reason().empty() && !recovering.Blocked(), "Noting a recovered fault must not change Turbo's status");
        bool archived = false;
        if (std::filesystem::exists(recovered_root / "faults")) {
            for (const auto& entry : std::filesystem::directory_iterator(recovered_root / "faults")) {
                const std::string text = read_file(entry.path());
                archived |= text.find("\"state\": \"recovered\"") != std::string::npos &&
                            text.find("Recovered from a runtime frame-call failure") != std::string::npos;
            }
        }
        Expect(archived, "A recovered fault must appear in fault history");
        // No Complete: a crash after the recovered fault.
    }
    {
        depthxr::TurboRecoveryGuard after_recovered;
        after_recovered.Configure(recovered_root, identity);
        Expect(!after_recovered.Prepare(true, false, "async", dead) && after_recovered.Blocked(),
               "A crash after a recovered fault must still block the next run");
    }

    // Fault history that cannot be saved costs the history record only.
    const auto unarchived_root = root / "recovered-unarchived";
    {
        depthxr::TurboRecoveryGuard recovering;
        recovering.Configure(unarchived_root, identity);
        Expect(recovering.Prepare(true, false, "async", dead), "Unarchived scenario should arm", recovering.Reason());
        const auto marker = find_marker(unarchived_root);
        Expect(!marker.empty(), "An armed guard has a marker");
        const std::string before = read_file(marker);
        std::ofstream(unarchived_root / "faults") << "file"; // Fault history cannot be a directory now.
        recovering.NoteRecovered("Recovered while history is unavailable");
        Expect(std::filesystem::is_regular_file(unarchived_root / "faults"), "The archive write should have failed");
        Expect(recovering.Reason().empty() && !recovering.Blocked(),
               "A lost history record must not report Turbo as off");
        Expect(read_file(marker) == before, "A lost history record must leave the marker armed");
    }
    {
        depthxr::TurboRecoveryGuard after_unarchived;
        after_unarchived.Configure(unarchived_root, identity);
        Expect(!after_unarchived.Prepare(true, false, "async", dead) && after_unarchived.Blocked(),
               "A crash after a lost history record must still block the next run");
    }

    const auto invalid_root = root / "not-a-directory";
    std::ofstream(invalid_root) << "file";
    depthxr::TurboRecoveryGuard unwritable;
    unwritable.Configure(invalid_root, identity);
    Expect(!unwritable.Prepare(true, false, "async", dead), "An unusable recovery path must fail safely");
    Expect(unwritable.Prepare(false, false, "async", dead), "Opt-out must work even with unavailable storage", unwritable.Reason());
    std::filesystem::remove_all(root);
    std::cout << "turbo_recovery_tests passed\n";
}
