#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

namespace depthxr {

// Mirrors OpenXR's implicit FIFO operations: wait targets the oldest acquired
// image that has not been waited, and release targets the oldest waited image.
// The downstream release can lag the app release while VectorXR composites.
class SwapchainImageQueue {
  public:
    void Acquire(uint32_t image_index);
    std::optional<uint32_t> WaitOldest();
    std::optional<uint32_t> ReleaseOldest();
    std::optional<uint32_t> ReleaseDownstreamOldest();
    void Clear();

    [[nodiscard]] size_t Size() const;
    [[nodiscard]] size_t PendingDownstreamReleases() const;

  private:
    struct ImageState {
        uint32_t index{0};
        bool waited{false};
        bool app_released{false};
        bool downstream_released{false};
    };

    void RetireReleasedPrefix();

    std::deque<ImageState> states_;
};

} // namespace depthxr
