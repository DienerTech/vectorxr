#include "depthxr/external_status.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <locale>
#include <sstream>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace {

void Expect(bool value, const std::string& message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

bool Contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

std::string ReadAll(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

bool Near(double a, double b) { return std::abs(a - b) < 1e-9; }

bool Removed(depthxr::ExternalStatusRemoval result) { return result == depthxr::ExternalStatusRemoval::kRemoved; }
bool NotOwned(depthxr::ExternalStatusRemoval result) { return result == depthxr::ExternalStatusRemoval::kNotOwned; }

depthxr::ExternalStatusDocument Identity(std::uint32_t process_id) {
    depthxr::ExternalStatusDocument document;
    document.process_id = process_id;
    document.application = "FlightSimulator2024.exe";
    document.session_id = std::to_string(process_id) + "-1785600000000-1";
    document.updated_at_unix_milliseconds = 1785600001234;
    return document;
}

// The document: identity always, the Frame Generation block only with it on,
// Auto's step only under Auto. Numbers in the classic locale whatever the
// game set.
void TestDocument() {
    depthxr::ExternalStatusDocument document = Identity(4242);
    std::string text = depthxr::FormatExternalStatus(document);
    Expect(Contains(text, "\"schema\": \"vectorxr.status\",\n"), "schema: " + text);
    Expect(Contains(text, "\"version\": 1,\n"), "version: " + text);
    Expect(Contains(text, "\"processId\": 4242,\n"), "process id: " + text);
    Expect(Contains(text, "\"application\": \"FlightSimulator2024.exe\",\n"), "application: " + text);
    Expect(Contains(text, "\"sessionId\": \"4242-1785600000000-1\",\n"), "session id: " + text);
    Expect(Contains(text, "\"updatedAtUnixMilliseconds\": 1785600001234\n}\n"), "update time, last: " + text);
    Expect(!Contains(text, "framegen"), "no Frame Generation block without it: " + text);

    // A fixed 2x hold at 90 Hz, the game keeping up.
    depthxr::ExternalFrameGenStatus framegen;
    framegen.state = "active";
    framegen.mode = "interpolate";
    framegen.frame_rate = "2x";
    framegen.generating = true;
    framegen.multiplier = 2;
    framegen.refresh_hz = 90.0;
    framegen.app_fps = 44.96;
    framegen.output_fps = 90.0;
    framegen.distinct_fps = 89.5;
    framegen.refreshes_per_real_frame = 2.004;
    depthxr::ExternalFrameGenTarget(&framegen);
    document.framegen = framegen;
    std::locale previous;
    try {
        previous = std::locale::global(std::locale("de-DE"));
    } catch (const std::exception&) {
        // Not installed: the classic locale is checked all the same.
    }
    text = depthxr::FormatExternalStatus(document);
    std::locale::global(previous);
    Expect(Contains(text, "\"updatedAtUnixMilliseconds\": 1785600001234,\n  \"framegen\": {\n"), "block follows: " + text);
    for (const char* field : {"\"state\": \"active\",", "\"mode\": \"interpolate\",", "\"frameRate\": \"2x\",",
                              "\"generating\": true,", "\"multiplier\": 2,", "\"refreshHz\": 90.00,",
                              "\"targetAppFps\": 45.00,", "\"appFps\": 44.96,", "\"outputFps\": 90.00,",
                              "\"distinctFps\": 89.50,", "\"refreshesPerRealFrame\": 2.00,", "\"holding\": true\n"}) {
        Expect(Contains(text, field), std::string("field ") + field + ": " + text);
    }
    Expect(!Contains(text, "autoStep"), "no Auto fields under a fixed rate: " + text);

    framegen.frame_rate = "auto";
    framegen.automatic = depthxr::ExternalAutoStep{2, 36.0, 76.5, 81.2};
    document.framegen = framegen;
    text = depthxr::FormatExternalStatus(document);
    Expect(Contains(text, "\"holding\": true,\n    \"autoStep\": 2,\n    \"autoStepUpBelowFps\": 36.00,\n"
                          "    \"autoStepDownAboveFps\": 76.50,\n    \"autoGameOwnFps\": 81.20\n  }\n}\n"),
           "Auto's step: " + text);

    // Text is escaped; a name cannot pass for the process id.
    document = Identity(7);
    document.application = "\"processId\": 9 \\ \x01";
    text = depthxr::FormatExternalStatus(document);
    Expect(Contains(text, "\"application\": \"\\\"processId\\\": 9 \\\\ \\u0001\""), "escaped: " + text);
}

// targetAppFps and holding, as the document defines them.
void TestTarget() {
    depthxr::ExternalFrameGenStatus framegen;
    framegen.generating = true;
    framegen.multiplier = 3;
    framegen.refresh_hz = 90.0;
    framegen.refreshes_per_real_frame = 3.2;
    depthxr::ExternalFrameGenTarget(&framegen);
    Expect(Near(framegen.target_app_fps, 30.0) && framegen.holding, "3x: 30 fps, held a slot missed now and then");
    framegen.refreshes_per_real_frame = 3.6;
    depthxr::ExternalFrameGenTarget(&framegen);
    Expect(Near(framegen.target_app_fps, 30.0) && !framegen.holding, "3x: a game at 25 fps does not hold 30");

    // Not generating (Auto at full rate, standing by, paused): the game may
    // run at the refresh rate, whatever the multiplier says.
    framegen.generating = false;
    framegen.multiplier = 1;
    framegen.refreshes_per_real_frame = 1.05;
    depthxr::ExternalFrameGenTarget(&framegen);
    Expect(Near(framegen.target_app_fps, 90.0) && framegen.holding, "full rate: 90 fps, held");
    framegen.multiplier = 2;
    framegen.refreshes_per_real_frame = 1.5;
    depthxr::ExternalFrameGenTarget(&framegen);
    Expect(Near(framegen.target_app_fps, 90.0) && !framegen.holding, "not generating: the refresh rate to hold");

    // Nothing known yet.
    framegen.refresh_hz = 0.0;
    framegen.refreshes_per_real_frame = 1.0;
    depthxr::ExternalFrameGenTarget(&framegen);
    Expect(framegen.target_app_fps == 0.0 && !framegen.holding, "no refresh rate: no target");
    framegen.refresh_hz = 90.0;
    framegen.refreshes_per_real_frame = 0.0;
    depthxr::ExternalFrameGenTarget(&framegen);
    Expect(Near(framegen.target_app_fps, 90.0) && !framegen.holding, "not measured: not holding");

    // The runtime holding frames to every second refresh, reporting that
    // period (Auto stays engaged there): a 2x frame takes two such periods,
    // four refreshes. The game held there aims for 22.5 fps and holds it.
    framegen.generating = true;
    framegen.multiplier = 2;
    framegen.refreshes_per_real_frame = 4.1;
    depthxr::ExternalFrameGenTarget(&framegen, 45.0);
    Expect(Near(framegen.target_app_fps, 22.5) && framegen.holding, "a halved grid: 22.5 fps, held");
    framegen.refreshes_per_real_frame = 5.0;
    depthxr::ExternalFrameGenTarget(&framegen, 45.0);
    Expect(!framegen.holding, "a halved grid: a game at 18 fps does not hold 22.5");
    framegen.generating = false;
    framegen.refreshes_per_real_frame = 2.0;
    depthxr::ExternalFrameGenTarget(&framegen, 45.0);
    Expect(Near(framegen.target_app_fps, 45.0) && framegen.holding, "a halved grid, passing through: 45 fps");
}

// The write replaces the file in one step: a reader polling it meanwhile
// only ever sees whole documents, and nothing is left beside it.
void TestAtomicWrite(const std::filesystem::path& root) {
    const auto path = root / "api" / "status.json";
    std::string error;
    Expect(depthxr::WriteExternalStatus(path, Identity(1), &error), "first write: " + error);
    Expect(ReadAll(path) == depthxr::FormatExternalStatus(Identity(1)), "the file holds the document");

    // The reader opens the file as the integrator page asks (all sharing,
    // delete too), so it never holds a replace up; polling far more often
    // than the heartbeat.
    std::atomic<bool> done{false};
    std::atomic<int> reads{0};
    std::atomic<int> partial{0};
    std::thread reader([&] {
        while (!done.load()) {
#if defined(_WIN32)
            HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                std::string text;
                char buffer[4096];
                DWORD read = 0;
                while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0) text.append(buffer, read);
                CloseHandle(file);
                ++reads;
                if (text.size() < 3 || text.front() != '{' || text.substr(text.size() - 2) != "}\n" ||
                    !Contains(text, "\"schema\": \"vectorxr.status\"")) {
                    ++partial;
                }
            }
#else
            const std::string text = ReadAll(path);
            ++reads;
            if (text.size() < 3 || text.front() != '{' || text.substr(text.size() - 2) != "}\n") ++partial;
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    int failed = 0;
    for (std::uint32_t index = 0; index < 300; ++index) {
        depthxr::ExternalStatusDocument document = Identity(index + 1);
        if (index % 2) {
            depthxr::ExternalFrameGenStatus framegen;
            framegen.state = "active";
            document.framegen = framegen;
        }
        if (!depthxr::WriteExternalStatus(path, document)) ++failed;
    }
    done = true;
    reader.join();
    Expect(reads > 0, "the reader read");
    Expect(partial == 0, "a reader saw part of a document (" + std::to_string(partial.load()) + " of " +
                             std::to_string(reads.load()) + ")");
    Expect(failed == 0, "writes failed beside a reader that shares the file: " + std::to_string(failed));
    Expect(depthxr::WriteExternalStatus(path, Identity(77), &error), "last write: " + error);
    Expect(ReadAll(path) == depthxr::FormatExternalStatus(Identity(77)), "the last write wins");
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(path.parent_path())) {
        (void)entry;
        ++files;
    }
    Expect(files == 1, "temporary files were left beside the status file");
}

// At session end a copy removes the file only while it names its process.
void TestOwnedRemoval(const std::filesystem::path& root) {
    const auto path = root / "owned" / "status.json";
    Expect(NotOwned(depthxr::RemoveExternalStatusIfOwned(path, 10)), "no file: nothing removed");
    std::string error;
    Expect(depthxr::WriteExternalStatus(path, Identity(20), &error), "write: " + error);
    Expect(NotOwned(depthxr::RemoveExternalStatusIfOwned(path, 10)), "another process's file is not removed");
    Expect(std::filesystem::exists(path) && ReadAll(path) == depthxr::FormatExternalStatus(Identity(20)),
           "another process's file stays as it was");
    Expect(NotOwned(depthxr::RemoveExternalStatusIfOwned(path, 2)),
           "a process id that is a prefix of the owner's is not it");
    Expect(Removed(depthxr::RemoveExternalStatusIfOwned(path, 20)), "the owner removes its file");
    Expect(!std::filesystem::exists(path), "the owner's file is gone");

    // Not a status document (part of one, another program's file): kept.
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << "{ \"schema\": \"vectorxr.status\", \"version\": 1, ";
    Expect(NotOwned(depthxr::RemoveExternalStatusIfOwned(path, 20)), "a file with no process id is not removed");
    Expect(std::filesystem::exists(path), "a file with no process id stays");

    // A name that holds the process id's text does not count: the identity is
    // the processId field itself.
    depthxr::ExternalStatusDocument named = Identity(30);
    named.application = "\"processId\": 20";
    Expect(depthxr::WriteExternalStatus(path, named, &error), "write: " + error);
    Expect(NotOwned(depthxr::RemoveExternalStatusIfOwned(path, 20)),
           "a process id in the application name is not the owner");
    Expect(Removed(depthxr::RemoveExternalStatusIfOwned(path, 30)), "the owner removes it");

#if defined(_WIN32)
    // A reader holding the file without delete sharing (as .NET's ReadAllText
    // does) only delays the removal.
    Expect(depthxr::WriteExternalStatus(path, Identity(40), &error), "write: " + error);
    HANDLE held = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    Expect(held != INVALID_HANDLE_VALUE, "could not hold the file open");
    std::thread release([held] {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        CloseHandle(held);
    });
    const bool removed = Removed(depthxr::RemoveExternalStatusIfOwned(path, 40));
    release.join();
    Expect(removed && !std::filesystem::exists(path), "a briefly held file is removed after a retry");

    // Held for longer than the retries: busy, to be tried again later.
    Expect(depthxr::WriteExternalStatus(path, Identity(45), &error), "write: " + error);
    HANDLE kept = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    Expect(kept != INVALID_HANDLE_VALUE, "could not hold the file open");
    Expect(depthxr::RemoveExternalStatusIfOwned(path, 45) == depthxr::ExternalStatusRemoval::kBusy,
           "a file held past the retries is reported busy");
    CloseHandle(kept);
    Expect(Removed(depthxr::RemoveExternalStatusIfOwned(path, 45)) && !std::filesystem::exists(path),
           "...and removed once it is let go");

    // A reader that shares delete does not keep the name: the next game's
    // write lands while the reader still has the old file open.
    Expect(depthxr::WriteExternalStatus(path, Identity(47), &error), "write: " + error);
    HANDLE sharing = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Expect(sharing != INVALID_HANDLE_VALUE, "could not open the file as a sharing reader");
    Expect(Removed(depthxr::RemoveExternalStatusIfOwned(path, 47)), "removed beside a sharing reader");
    Expect(depthxr::WriteExternalStatus(path, Identity(48), &error), "a write beside the old file's reader: " + error);
    CloseHandle(sharing);
    Expect(ReadAll(path) == depthxr::FormatExternalStatus(Identity(48)), "the next game's file is there");
    std::filesystem::remove(path);

    // The sharing the removal relies on, by hand: while a handle opened as
    // it opens the file is held, another game's write waits for it (its
    // retries) and then lands, so no write slips between the check and the
    // removal. (Timing only makes the write likely to start meanwhile.)
    Expect(depthxr::WriteExternalStatus(path, Identity(50), &error), "write: " + error);
    HANDLE removing = CreateFileW(path.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    Expect(removing != INVALID_HANDLE_VALUE, "could not open the file as the removal does");
    std::thread writer([&] { depthxr::WriteExternalStatus(path, Identity(60)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    Expect(SetFileInformationByHandle(removing, FileDispositionInfo, &disposition, sizeof(disposition)) != FALSE,
           "could not mark the file for removal");
    CloseHandle(removing);
    writer.join();
    Expect(std::filesystem::exists(path) && ReadAll(path) == depthxr::FormatExternalStatus(Identity(60)),
           "the other game's write landed after the removal");
#endif
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("vectorxr-external-status-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    TestDocument();
    TestTarget();
    TestAtomicWrite(root);
    TestOwnedRemoval(root);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::cout << "external_status_tests passed\n";
    return 0;
}
