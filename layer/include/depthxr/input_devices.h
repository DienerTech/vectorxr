#pragma once

#include "depthxr/settings.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace depthxr {

enum class DeviceInputKind {
    Button,
    Hat,
};

struct DeviceInputPath {
    DeviceInputKind kind{DeviceInputKind::Button};
    std::size_t index{0};
    std::size_t direction{0};
};

enum class InputBindingPollStage {
    None,
    ParseInputPath,
    ParseDeviceGuid,
    CreateDirectInput,
    CreateDevice,
    SetDataFormat,
    FindTopLevelWindow,
    SetCooperativeLevel,
    Acquire,
    Poll,
    GetDeviceState,
};

struct InputBindingPollResult {
    bool down{false};
    bool device_poll_attempted{false};
    // True when a known-unavailable device was left inactive without another
    // DirectInput call. This preserves diagnostics while preventing reconnect
    // attempts from running at the frame-loop polling rate.
    bool device_retry_deferred{false};
    std::int64_t device_retry_delay_ms{0};
    InputBindingPollStage diagnostic_stage{InputBindingPollStage::None};
    std::int64_t result_code{0};
    bool reacquire_attempted{false};
    std::int64_t reacquire_result_code{0};
    bool retry_attempted{false};
    std::int64_t retry_result_code{0};
    bool recovered{false};
    std::uintptr_t cooperative_window{0};
};

// Per-device reconnect policy used after DirectInput setup/read failures.
// The poller owns synchronization; this small policy object is deliberately
// clock-driven so its storm-prevention behavior can be tested deterministically.
class InputDeviceRetryBackoff {
  public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit InputDeviceRetryBackoff(
        std::chrono::milliseconds initial_delay = std::chrono::milliseconds{250},
        std::chrono::milliseconds maximum_delay = std::chrono::milliseconds{2000});

    bool ShouldAttempt(const std::wstring& device_key, TimePoint now) const;
    std::chrono::milliseconds RecordFailure(const std::wstring& device_key, TimePoint now);
    void RecordSuccess(const std::wstring& device_key);
    std::chrono::milliseconds RetryDelayRemaining(const std::wstring& device_key,
                                                   TimePoint now) const;
    std::size_t ConsecutiveFailures(const std::wstring& device_key) const;

  private:
    struct Entry {
        TimePoint retry_after{};
        std::chrono::milliseconds delay{0};
        std::size_t consecutive_failures{0};
    };

    std::chrono::milliseconds initial_delay_;
    std::chrono::milliseconds maximum_delay_;
    std::unordered_map<std::wstring, Entry> entries_;
};

std::optional<DeviceInputPath> ParseDeviceInputPath(std::string_view input_path);
std::optional<std::size_t> DirectInputHatDirection(std::uint32_t value);
const char* ToString(InputBindingPollStage stage);
const char* DirectInputResultName(std::int64_t result_code);

InputBindingPollResult PollInputBinding(const InputBinding& binding);
bool IsInputBindingDown(const InputBinding& binding);

} // namespace depthxr
