#pragma once

#include "depthxr/settings.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

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
    // True while the device's DirectInput setup runs on the background
    // connector. The binding reads inactive until the device is published.
    bool device_connect_pending{false};
    // The device is not connected and, during a session, is not retried on a
    // timer: it is tried again when Windows reports a device's arrival, when
    // the VectorXR app's Rescan inputs asks, or at the next session's start.
    bool device_waits_for_rescan{false};
    // Windows reports device arrivals to the layer (false: only Rescan
    // inputs and a session's start find a device plugged in later).
    bool device_arrival_notified{false};
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
    // A device DirectInput does not know (unplugged, or never there). During
    // a session it is not retried on the timer: an attempt every 2 s stalled
    // the game with it (Star Citizen, 2026-10-18: a 48-70 ms hitch at each
    // attempt). It waits for Rescan; outside a session the timer still runs.
    // `attempted_after` is Rescans() when the attempt began (the connector's
    // runs off the frame thread): a rescan while it ran, such as a controller
    // arriving while DirectInput looked for it, gets the device tried again.
    // Without it, the attempt is taken to have begun at the latest rescan.
    std::chrono::milliseconds RecordAbsent(const std::wstring& device_key, TimePoint now);
    std::chrono::milliseconds RecordAbsent(const std::wstring& device_key, TimePoint now,
                                           std::uint64_t attempted_after);
    // How many rescans there have been (a device's arrival, Rescan inputs, a
    // session's start).
    std::uint64_t Rescans() const { return rescans_; }
    void RecordSuccess(const std::wstring& device_key);
    std::chrono::milliseconds RetryDelayRemaining(const std::wstring& device_key,
                                                   TimePoint now) const;
    std::size_t ConsecutiveFailures(const std::wstring& device_key) const;
    // The game's OpenXR session runs (absent devices wait for Rescan).
    void SetSessionActive(bool active);
    bool SessionActive() const { return session_active_; }
    // Lets each absent device be tried once more: a device's arrival, the
    // app's Rescan inputs, a session's start.
    void Rescan();
    bool WaitsForRescan(const std::wstring& device_key) const;

  private:
    struct Entry {
        TimePoint retry_after{};
        std::chrono::milliseconds delay{0};
        std::size_t consecutive_failures{0};
        bool absent{false};
        std::uint64_t rescan_seen{0}; // the rescan it was found absent after
    };

    std::chrono::milliseconds Delay(Entry& entry, TimePoint now);

    std::chrono::milliseconds initial_delay_;
    std::chrono::milliseconds maximum_delay_;
    std::unordered_map<std::wstring, Entry> entries_;
    bool session_active_{false};
    std::uint64_t rescans_{0};
};

// A device published by the background connector may already have inputs held
// (for example a maintained HOTAS switch). Callers prime their edge detectors
// on their first poll, which reads inactive while the connector runs, so an
// input held at connect reads released until it is released once; otherwise
// the connect itself would look like a press. The poller owns synchronization.
class InputConnectPriming {
  public:
    void DeviceConnected(const std::wstring& device_key);
    bool Filter(const std::wstring& device_key, std::string_view input_path, bool down);

  private:
    struct Device {
        std::uint64_t generation{0};
        // Input path -> connection generation in which it was seen released.
        std::vector<std::pair<std::string, std::uint64_t>> released;
    };
    std::unordered_map<std::wstring, Device> devices_;
};

std::optional<DeviceInputPath> ParseDeviceInputPath(std::string_view input_path);
std::optional<std::size_t> DirectInputHatDirection(std::uint32_t value);
const char* ToString(InputBindingPollStage stage);
const char* DirectInputResultName(std::int64_t result_code);

// Device bindings never set up DirectInput devices on the calling (frame)
// thread: CreateDevice/Acquire/Release cost 1-6ms per device and a failing
// device retries on the reconnect backoff, which showed up as a periodic
// frame-time spike. Setup runs on a short-lived background connector instead.
InputBindingPollResult PollInputBinding(const InputBinding& binding);
bool IsInputBindingDown(const InputBinding& binding);
// Waits (bounded) for in-flight background device setup, and stops Windows'
// device-arrival notifications. Call before the layer can be unloaded; never
// from the frame thread.
void DrainInputDeviceWork(std::chrono::milliseconds timeout);
// The game's OpenXR session started (devices not connected are tried once,
// then wait for a rescan) or ended (they retry on the reconnect timer).
void SetInputDeviceSessionActive(bool active);
// The VectorXR app's Rescan inputs: devices not connected are tried once more.
void RescanInputDevices();

} // namespace depthxr
