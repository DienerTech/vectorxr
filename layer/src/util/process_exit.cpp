#include "depthxr/process_exit.h"

#include <array>
#include <atomic>
#include <cstddef>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace depthxr {
namespace {

// A session arms at most a Turbo recovery marker and its two relay files;
// the rest is headroom for processes that make several sessions.
constexpr std::size_t kExitFileSlots = 16;
constexpr std::size_t kExitPathCapacity = 1024;

struct ExitFileSlot {
    std::atomic<bool> taken{false};
    std::atomic<bool> armed{false};
    wchar_t path[kExitPathCapacity]{};
};

// Constant-initialized: usable from DllMain whatever the static init order.
std::array<ExitFileSlot, kExitFileSlots> g_exit_files;
std::atomic<bool> g_process_exiting{false};

} // namespace

void MarkProcessExiting() {
    g_process_exiting.store(true, std::memory_order_release);
}

bool ProcessExiting() {
    return g_process_exiting.load(std::memory_order_acquire);
}

ProcessExitFile::~ProcessExitFile() {
    Disarm();
}

bool ProcessExitFile::Arm(const std::filesystem::path& path) {
    const auto& text = path.native();
    if (text.empty() || text.size() >= kExitPathCapacity) {
        Disarm();
        return false;
    }
    if (slot_ < 0) {
        for (std::size_t index = 0; index < g_exit_files.size(); ++index) {
            bool expected = false;
            if (g_exit_files[index].taken.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                slot_ = static_cast<int>(index);
                break;
            }
        }
        if (slot_ < 0) return false;
    }
    ExitFileSlot& slot = g_exit_files[static_cast<std::size_t>(slot_)];
    // Disarmed while the text changes: a thread ended mid-copy leaves a
    // disarmed slot, never a half-written path for the exit to delete.
    slot.armed.exchange(false, std::memory_order_acq_rel);
    for (std::size_t index = 0; index < text.size(); ++index) slot.path[index] = static_cast<wchar_t>(text[index]);
    slot.path[text.size()] = L'\0';
    slot.armed.store(true, std::memory_order_release);
    return true;
}

void ProcessExitFile::Disarm() {
    if (slot_ < 0) return;
    ExitFileSlot& slot = g_exit_files[static_cast<std::size_t>(slot_)];
    slot.armed.store(false, std::memory_order_release);
    slot.taken.store(false, std::memory_order_release);
    slot_ = -1;
}

void RemoveProcessExitFiles() {
    for (ExitFileSlot& slot : g_exit_files) {
        if (!slot.armed.exchange(false, std::memory_order_acq_rel)) continue;
#if defined(_WIN32)
        DeleteFileW(slot.path);
#endif
    }
}

} // namespace depthxr
