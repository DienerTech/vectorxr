#pragma once

// Fences the GPU tests hold queues back with ("gates"), until the test opens
// them. A failed check exits at once, without destructors, so it first opens
// every gate still shut: no test process exits with a queue waiting on the
// GPU for a fence nothing will signal.

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <vector>

namespace test_gates {

// Past any value a test waits for (and within a timeline semaphore's reach).
inline constexpr UINT64 kOpenValue = 1ull << 20;

struct Registry {
    std::timed_mutex mutex;
    std::vector<ID3D12Fence*> fences;
};

// Never destroyed: a failure may exit from any thread.
inline Registry& Gates() {
    static Registry* registry = new Registry;
    return *registry;
}

// While a gate is registered its fence is kept alive by its owner.
inline void Add(ID3D12Fence* fence) {
    std::scoped_lock lock(Gates().mutex);
    Gates().fences.push_back(fence);
}

inline void Remove(ID3D12Fence* fence) {
    std::scoped_lock lock(Gates().mutex);
    std::vector<ID3D12Fence*>& fences = Gates().fences;
    fences.erase(std::remove(fences.begin(), fences.end(), fence), fences.end());
}

// Called by a failed check before it exits: every gate is opened and the
// queues get a moment to run on.
inline void OpenAllBeforeExit() {
    std::unique_lock lock(Gates().mutex, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::seconds(1))) {
        return;
    }
    bool opened = false;
    for (ID3D12Fence* fence : Gates().fences) {
        if (fence->GetCompletedValue() < kOpenValue && SUCCEEDED(fence->Signal(kOpenValue))) {
            opened = true;
        }
    }
    if (opened) {
        Sleep(200);
    }
}

// A fence registered as a gate for as long as this lives (declared after the
// fence, so it goes first).
class Guard {
public:
    explicit Guard(ID3D12Fence* fence) : fence_(fence) {
        if (fence_) Add(fence_);
    }
    ~Guard() {
        if (fence_) Remove(fence_);
    }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

private:
    ID3D12Fence* fence_;
};

} // namespace test_gates
