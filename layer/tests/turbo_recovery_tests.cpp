#include "depthxr/turbo_recovery.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

void Expect(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}

int main() {
    const auto root = std::filesystem::temp_directory_path() / ("vectorxr-recovery-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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
        Expect(first.Prepare(true, false, "async", dead), "First run should arm");
        // Intentionally no Complete: simulate a crash without destructor cleanup.
    }
    depthxr::TurboRecoveryGuard concurrent;
    concurrent.Configure(root, identity);
    Expect(concurrent.Prepare(true, false, "async", alive), "A live process is not an interrupted run");
    concurrent.Complete();

    depthxr::TurboRecoveryGuard next;
    next.Configure(root, identity);
    Expect(!next.Prepare(true, false, "async", dead) && next.Blocked(), "Dead process should block the next run");
    next.Complete();
    Expect(!next.Prepare(true, false, "async", dead), "A clean Turbo-off run must preserve the interruption");
    Expect(next.Prepare(false, false, "async", dead), "Opt-out must bypass recovery");
    Expect(!next.Prepare(true, false, "async", dead), "Re-enabling recovery must restore the unresolved interruption");

    auto other_identity = identity;
    other_identity.fingerprint = "other:steamvr:d3d11";
    depthxr::TurboRecoveryGuard other;
    other.Configure(root, other_identity);
    Expect(other.Prepare(true, false, "async", dead), "An interruption must not block a different app/setup");
    other.Complete();

    Expect(next.Prepare(true, true, "sequenced", dead), "Explicit retry should supersede the previous interruption");
    next.Complete();
    depthxr::TurboRecoveryGuard clean;
    clean.Configure(root, identity);
    Expect(clean.Prepare(true, false, "async", dead), "A clean retry should clear recovery history");
    clean.Fail("Repeated runtime submission errors");
    clean.Complete();
    depthxr::TurboRecoveryGuard failed;
    failed.Configure(root, identity);
    Expect(!failed.Prepare(true, false, "async", alive), "Runtime failures survive clean shutdown and do not depend on process death");
    Expect(failed.Prepare(true, true, "async", dead), "Failure recovery can be explicitly retried");
    failed.Prepare(false, false, "async", dead);
    depthxr::TurboRecoveryGuard bypassed;
    bypassed.Configure(root, identity);
    Expect(bypassed.Prepare(true, false, "async", dead), "Disabling the watch during a run removes its own marker");
    bypassed.Complete();

    Expect(std::filesystem::exists(root / "faults"), "Retries must retain fault history");
    const auto fault_count = std::distance(std::filesystem::directory_iterator(root / "faults"), std::filesystem::directory_iterator());
    Expect(fault_count >= 2, "Crash and runtime failure must both be archived");
    depthxr::TurboRecoveryGuard ui_clear;
    ui_clear.Configure(root, identity);
    Expect(ui_clear.Prepare(true, false, "async", dead), "UI clear test should start clean");
    ui_clear.Fail("UI clear test failure");
    // Model the UI archive-and-remove action. Faults already archived by Fail.
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.path().extension() == ".json") std::filesystem::remove(entry.path());
    }
    Expect(ui_clear.ConsumeClearRequest(), "A live suspended session should observe UI clearing its failed marker");
    Expect(ui_clear.Prepare(true, true, "async", dead), "UI clear should allow the session to re-arm");
    ui_clear.Complete();
    Expect(std::distance(std::filesystem::directory_iterator(root / "faults"), std::filesystem::directory_iterator()) > fault_count,
           "UI clear must retain the failure details");
    depthxr::TurboRecoveryGuard opted_out;
    opted_out.Configure(root, identity);
    opted_out.Prepare(false, false, "sequenced", dead);
    opted_out.Fail("Fault with safety bypassed");
    depthxr::TurboRecoveryGuard after_opt_out;
    after_opt_out.Configure(root, identity);
    Expect(after_opt_out.Prepare(true, false, "async", dead), "Opt-out diagnostics must not create a block");
    after_opt_out.Complete();

    const auto clear_root = root / "clear-logs";
    depthxr::TurboRecoveryGuard logged;
    logged.Configure(clear_root, identity);
    Expect(logged.Prepare(true, false, "async", dead), "Clear log scenario should arm");
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
    Expect(still_blocked.Prepare(true, true, "async", dead), "Cleared fault can still be explicitly retried");
    Expect(std::filesystem::is_empty(clear_root / "faults"), "Retry must not resurrect cleared fault history");
    still_blocked.Fail("New fault after clearing");
    Expect(!std::filesystem::is_empty(clear_root / "faults"), "New faults must still be archived");

    const auto invalid_root = root / "not-a-directory";
    std::ofstream(invalid_root) << "file";
    depthxr::TurboRecoveryGuard unwritable;
    unwritable.Configure(invalid_root, identity);
    Expect(!unwritable.Prepare(true, false, "async", dead), "An unusable recovery path must fail safely");
    Expect(unwritable.Prepare(false, false, "async", dead), "Opt-out must work even with unavailable storage");
    std::filesystem::remove_all(root);
    std::cout << "turbo_recovery_tests passed\n";
}
