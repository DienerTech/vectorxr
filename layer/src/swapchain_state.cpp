#include "depthxr/swapchain_state.h"

#include <algorithm>

namespace depthxr {

void SwapchainImageQueue::Acquire(uint32_t image_index) {
    states_.push_back({image_index, false, false, false});
}

std::optional<uint32_t> SwapchainImageQueue::WaitOldest() {
    const auto pending = std::find_if(states_.begin(), states_.end(), [](const ImageState& state) {
        return !state.waited;
    });
    if (pending == states_.end()) {
        return std::nullopt;
    }
    pending->waited = true;
    return pending->index;
}

std::optional<uint32_t> SwapchainImageQueue::ReleaseOldest() {
    const auto pending = std::find_if(states_.begin(), states_.end(), [](const ImageState& state) {
        return state.waited && !state.app_released;
    });
    if (pending == states_.end()) {
        return std::nullopt;
    }
    pending->app_released = true;
    return pending->index;
}

std::optional<uint32_t> SwapchainImageQueue::ReleaseDownstreamOldest() {
    const auto pending = std::find_if(states_.begin(), states_.end(), [](const ImageState& state) {
        return state.app_released && !state.downstream_released;
    });
    if (pending == states_.end()) {
        return std::nullopt;
    }
    pending->downstream_released = true;
    const uint32_t index = pending->index;
    RetireReleasedPrefix();
    return index;
}

void SwapchainImageQueue::Clear() {
    states_.clear();
}

size_t SwapchainImageQueue::Size() const {
    return states_.size();
}

size_t SwapchainImageQueue::PendingDownstreamReleases() const {
    return static_cast<size_t>(std::count_if(states_.begin(), states_.end(), [](const ImageState& state) {
        return state.app_released && !state.downstream_released;
    }));
}

void SwapchainImageQueue::RetireReleasedPrefix() {
    while (!states_.empty() && states_.front().downstream_released) {
        states_.pop_front();
    }
}

} // namespace depthxr
