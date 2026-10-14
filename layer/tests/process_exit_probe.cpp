// A stand-in for the layer DLL in process_exit_tests: the layer's own DllMain
// (dll_main.cpp) plus one armed exit file, so a child process can show what a
// normal exit, an unload and a TerminateProcess do to it.
#include <Windows.h>

#include <filesystem>
#include <fstream>

#include "depthxr/logger.h"
#include "depthxr/process_exit.h"

namespace {

std::filesystem::path g_sentinel;

// Destroyed by the CRT after DllMain's DLL_PROCESS_DETACH, like the layer's
// statics: it records whether ProcessExiting() was already set by then.
struct ExitObserver {
    ~ExitObserver() {
        if (!g_sentinel.empty()) {
            std::ofstream(g_sentinel) << (depthxr::ProcessExiting() ? "exiting" : "unloading");
        }
    }
};

ExitObserver g_observer;
depthxr::ProcessExitFile g_exit_file;

// Like the layer's logger: never destroyed at a process exit, only flushed
// by a static destructor that sees ProcessExiting().
depthxr::Logger* g_log = nullptr;
struct ExitLogFlush {
    ~ExitLogFlush() {
        if (g_log && depthxr::ProcessExiting()) g_log->FlushAtProcessExit();
    }
};
ExitLogFlush g_exit_log_flush;

} // namespace

extern "C" __declspec(dllexport) int ArmProcessExitFile(const wchar_t* path, const wchar_t* sentinel) {
    g_sentinel = sentinel;
    return g_exit_file.Arm(path) ? 1 : 0;
}

// A line, then the same line three times: the repeats stay pending (and the
// last lines buffered) until the logger writes its summary.
extern "C" __declspec(dllexport) void WriteExitLog(const wchar_t* path) {
    g_log = new depthxr::Logger();
    g_log->Initialize(path);
    g_log->Info("first line");
    for (int i = 0; i < 3; ++i) g_log->Info("repeated line");
}
