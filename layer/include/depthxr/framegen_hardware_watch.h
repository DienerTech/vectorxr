#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace depthxr {

// One producer under the GPU lock; the watchdog reads snapshots concurrently.
// Fences must outlive their snapshots (registered OF fences are kept alive).
template <typename Fence, std::size_t Count = 4>
class FrameGenHardwareWatch {
  public:
    struct Snapshot {
        Fence* fence{nullptr};
        std::uint64_t done{0};
        std::uint64_t input{0};
        std::uint64_t allowed{0};
        std::uint64_t epoch{0};
        std::size_t index{0};
        explicit operator bool() const { return fence && done; }
    };

    void Watch(std::uint64_t done, std::uint64_t input, std::uint64_t allowed) {
        Operation& operation = operations_[done % Count];
        operation.done = 0;
        operation.input = input;
        operation.allowed = allowed;
        operation.done = done;
    }

    Snapshot Read(std::size_t index) const {
        const std::uint64_t epoch = epoch_;
        if (epoch & 1u) return {};
        const Operation& operation = operations_[index];
        const Snapshot result{fence_, operation.done, operation.input, operation.allowed, epoch, index};
        return Current(result) ? result : Snapshot{};
    }

    bool Current(const Snapshot& snapshot) const {
        return !(snapshot.epoch & 1u) && epoch_ == snapshot.epoch && fence_ == snapshot.fence &&
               operations_[snapshot.index].done == snapshot.done && epoch_ == snapshot.epoch;
    }

    // Input readiness is irrelevant here: queued operations need their watch
    // too, or a wait ahead of their input could never be released.
    bool Pending() const {
        Fence* active = fence_;
        if (!active) return false;
        const std::uint64_t completed = active->GetCompletedValue();
        for (const Operation& operation : operations_) {
            const std::uint64_t done = operation.done;
            if (done && completed < done) return true;
        }
        return false;
    }

    // Only the producer calls this. Never discard an unfinished operation.
    bool ResetFence(Fence* replacement) {
        if (Pending()) return false;
        ++epoch_; // odd: snapshots taken during replacement are invalid
        for (Operation& operation : operations_) operation.done = 0;
        fence_ = replacement;
        ++epoch_;
        return true;
    }

    Fence* ActiveFence() const { return fence_; }

  private:
    struct Operation {
        std::atomic<std::uint64_t> done{0};
        std::atomic<std::uint64_t> input{0};
        std::atomic<std::uint64_t> allowed{250};
    };
    std::atomic<Fence*> fence_{nullptr};
    std::atomic<std::uint64_t> epoch_{0};
    std::array<Operation, Count> operations_;
};

} // namespace depthxr
