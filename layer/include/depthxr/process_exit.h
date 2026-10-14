#pragma once

#include <filesystem>

namespace depthxr {

// How the layer sees its process end. A process that exits normally
// (ExitProcess: returning from main, exit()) ends every other thread first,
// then sends DLL_PROCESS_DETACH to each DLL under the loader lock, and only
// then runs this DLL's static destructors. A crash, TerminateProcess or a
// fail-fast skips all of it. Star Citizen and MSFS exit without OpenXR
// teardown, so DllMain is the only place a normal exit of theirs shows.

// Set by DllMain at a DLL_PROCESS_DETACH whose process is exiting, before the
// CRT runs static destructors. Those must then not lock or wait on anything an
// ended thread may hold, nor call into other modules: a contended lock ends
// the process on the spot (0xC000004B), a wait for an ended thread never
// returns, and modules loaded after this layer (a runtime's, a driver's)
// have already run their own DLL_PROCESS_DETACH.
void MarkProcessExiting();
bool ProcessExiting();

// A file that must not outlive a normal process exit. While armed, the exit
// removes it; a crash or forced termination leaves it for the next launch.
// Armed paths live in a fixed table, so the removal at exit takes no lock and
// allocates nothing. Each object serializes its own calls.
class ProcessExitFile {
public:
    ProcessExitFile() = default;
    ~ProcessExitFile(); // Disarms; never removes the file.
    ProcessExitFile(const ProcessExitFile&) = delete;
    ProcessExitFile& operator=(const ProcessExitFile&) = delete;

    // False when the path is too long or the table is full: the file then
    // outlives the process, as it did before.
    bool Arm(const std::filesystem::path& path);
    void Disarm();

private:
    int slot_{-1};
};

// DllMain at a process exit (and tests): one DeleteFileW per armed file.
void RemoveProcessExitFiles();

} // namespace depthxr
