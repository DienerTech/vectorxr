#include "depthxr/input_devices.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#include <dinput.h>
#include <objbase.h>
#endif

namespace depthxr {

std::optional<DeviceInputPath> ParseDeviceInputPath(std::string_view input_path) {
    const auto parse_index = [](std::string_view text, std::size_t maximum) -> std::optional<std::size_t> {
        if (text.empty() ||
            !std::all_of(text.begin(), text.end(), [](unsigned char character) {
                return std::isdigit(character) != 0;
            })) {
            return std::nullopt;
        }

        std::size_t value = 0;
        for (const char character : text) {
            value = value * 10 + static_cast<std::size_t>(character - '0');
            if (value > maximum) {
                return std::nullopt;
            }
        }
        if (value < 1) {
            return std::nullopt;
        }
        return value - 1;
    };

    constexpr std::string_view button_prefix = "button-";
    if (input_path.starts_with(button_prefix)) {
        const std::optional<std::size_t> index =
            parse_index(input_path.substr(button_prefix.size()), 128);
        if (index.has_value()) {
            return DeviceInputPath{DeviceInputKind::Button, *index, 0};
        }
        return std::nullopt;
    }

    constexpr std::string_view hat_prefix = "hat-";
    if (!input_path.starts_with(hat_prefix)) {
        return std::nullopt;
    }

    const std::string_view remainder = input_path.substr(hat_prefix.size());
    const std::size_t separator = remainder.find('-');
    if (separator == std::string_view::npos) {
        return std::nullopt;
    }

    const std::optional<std::size_t> index = parse_index(remainder.substr(0, separator), 4);
    constexpr std::array<std::string_view, 8> directions = {
        "up", "up-right", "right", "down-right",
        "down", "down-left", "left", "up-left",
    };
    const auto direction = std::find(directions.begin(), directions.end(), remainder.substr(separator + 1));
    if (!index.has_value() || direction == directions.end()) {
        return std::nullopt;
    }

    return DeviceInputPath{
        DeviceInputKind::Hat,
        *index,
        static_cast<std::size_t>(std::distance(directions.begin(), direction)),
    };
}

std::optional<std::size_t> DirectInputHatDirection(std::uint32_t value) {
    if (value == UINT32_MAX) {
        return std::nullopt;
    }
    return static_cast<std::size_t>((((value % 36'000) + 2'250) / 4'500) % 8);
}

const char* ToString(InputBindingPollStage stage) {
    switch (stage) {
    case InputBindingPollStage::None:
        return "none";
    case InputBindingPollStage::ParseInputPath:
        return "parse-input-path";
    case InputBindingPollStage::ParseDeviceGuid:
        return "parse-device-guid";
    case InputBindingPollStage::CreateDirectInput:
        return "create-direct-input";
    case InputBindingPollStage::CreateDevice:
        return "create-device";
    case InputBindingPollStage::SetDataFormat:
        return "set-data-format";
    case InputBindingPollStage::FindTopLevelWindow:
        return "find-top-level-window";
    case InputBindingPollStage::SetCooperativeLevel:
        return "set-cooperative-level";
    case InputBindingPollStage::Acquire:
        return "acquire";
    case InputBindingPollStage::Poll:
        return "poll";
    case InputBindingPollStage::GetDeviceState:
        return "get-device-state";
    default:
        return "unknown";
    }
}

const char* DirectInputResultName(std::int64_t result_code) {
#if defined(_WIN32)
    const HRESULT result = static_cast<HRESULT>(result_code);
    if (result == DI_OK) {
        return "DI_OK";
    }
    if (result == S_FALSE) {
        return "S_FALSE";
    }
    if (result == E_HANDLE) {
        return "E_HANDLE";
    }
    if (result == E_INVALIDARG) {
        return "E_INVALIDARG";
    }
    if (result == E_POINTER) {
        return "E_POINTER";
    }
    if (result == DIERR_INPUTLOST) {
        return "DIERR_INPUTLOST";
    }
    if (result == DIERR_NOTACQUIRED) {
        return "DIERR_NOTACQUIRED";
    }
    if (result == DIERR_OTHERAPPHASPRIO) {
        return "DIERR_OTHERAPPHASPRIO";
    }
    if (result == DIERR_NOTINITIALIZED) {
        return "DIERR_NOTINITIALIZED";
    }
    if (result == DIERR_DEVICENOTREG) {
        return "DIERR_DEVICENOTREG";
    }
    if (result == DIERR_UNPLUGGED) {
        return "DIERR_UNPLUGGED";
    }
#else
    (void)result_code;
#endif
    return "unknown";
}

InputDeviceRetryBackoff::InputDeviceRetryBackoff(
    std::chrono::milliseconds initial_delay,
    std::chrono::milliseconds maximum_delay)
    : initial_delay_(std::max(initial_delay, std::chrono::milliseconds{1})),
      maximum_delay_(std::max(maximum_delay, initial_delay_)) {}

bool InputDeviceRetryBackoff::ShouldAttempt(const std::wstring& device_key,
                                            TimePoint now) const {
    const auto entry = entries_.find(device_key);
    return entry == entries_.end() || now >= entry->second.retry_after;
}

std::chrono::milliseconds InputDeviceRetryBackoff::RecordFailure(
    const std::wstring& device_key,
    TimePoint now) {
    Entry& entry = entries_[device_key];
    if (entry.consecutive_failures == 0) {
        entry.delay = initial_delay_;
    } else {
        entry.delay = std::min(entry.delay * 2, maximum_delay_);
    }
    ++entry.consecutive_failures;
    entry.retry_after = now + entry.delay;
    return entry.delay;
}

void InputDeviceRetryBackoff::RecordSuccess(const std::wstring& device_key) {
    entries_.erase(device_key);
}

std::chrono::milliseconds InputDeviceRetryBackoff::RetryDelayRemaining(
    const std::wstring& device_key,
    TimePoint now) const {
    const auto entry = entries_.find(device_key);
    if (entry == entries_.end() || now >= entry->second.retry_after) {
        return std::chrono::milliseconds{0};
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        entry->second.retry_after - now);
}

std::size_t InputDeviceRetryBackoff::ConsecutiveFailures(
    const std::wstring& device_key) const {
    const auto entry = entries_.find(device_key);
    return entry == entries_.end() ? 0 : entry->second.consecutive_failures;
}

void InputConnectPriming::DeviceConnected(const std::wstring& device_key) {
    ++devices_[device_key].generation;
}

bool InputConnectPriming::Filter(const std::wstring& device_key, std::string_view input_path, bool down) {
    const auto device = devices_.find(device_key);
    if (device == devices_.end()) {
        return down;
    }
    auto input = std::find_if(device->second.released.begin(), device->second.released.end(),
                              [&](const auto& entry) { return entry.first == input_path; });
    if (input == device->second.released.end()) {
        device->second.released.emplace_back(std::string(input_path), 0);
        input = std::prev(device->second.released.end());
    }
    if (input->second == device->second.generation) {
        return down;
    }
    if (!down) {
        input->second = device->second.generation;
    }
    return false;
}

namespace {

#if defined(_WIN32)
std::int64_t ResultCode(HRESULT result) {
    return static_cast<std::int64_t>(static_cast<std::int32_t>(result));
}

std::optional<int> ToVirtualKey(std::string_view key) {
    if (key == "Space") {
        return VK_SPACE;
    }
    if (key == "Ctrl") {
        return VK_CONTROL;
    }
    if (key == "Alt") {
        return VK_MENU;
    }
    if (key == "Shift") {
        return VK_SHIFT;
    }

    if (key.size() == 7 && key.starts_with("Numpad") && key[6] >= '0' && key[6] <= '9') {
        return VK_NUMPAD0 + (key[6] - '0');
    }

    if (key.size() == 1) {
        const char character = key[0];
        if ((character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9')) {
            return static_cast<int>(character);
        }
    }

    if (key.size() >= 2 && key[0] == 'F') {
        const std::string suffix(key.substr(1));
        const int function_key = std::stoi(suffix);
        if (function_key >= 1 && function_key <= 12) {
            return VK_F1 + (function_key - 1);
        }
    }

    return std::nullopt;
}

bool IsVirtualKeyDown(int virtual_key) {
    return (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
}
#endif

bool IsKeyboardBindingDown(const InputBinding& binding) {
    if (binding.type != InputBindingType::Keyboard) {
        return false;
    }

#if defined(_WIN32)
    std::vector<int> virtual_keys;
    virtual_keys.reserve(binding.chord.size());
    for (const std::string& key : binding.chord) {
        const std::optional<int> virtual_key = ToVirtualKey(key);
        if (!virtual_key.has_value()) {
            return false;
        }
        virtual_keys.push_back(*virtual_key);
    }

    if (virtual_keys.empty()) {
        return false;
    }

    return std::all_of(virtual_keys.begin(), virtual_keys.end(), IsVirtualKeyDown);
#else
    return false;
#endif
}

#if defined(_WIN32)
std::wstring Widen(std::string_view value) {
    return std::wstring(value.begin(), value.end());
}

std::wstring NormalizeGuidText(std::string_view value) {
    std::wstring text = Widen(value);
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return text;
}

std::optional<GUID> ParseGuid(std::string_view value) {
    const std::wstring wide = Widen(value);
    GUID guid{};
    if (SUCCEEDED(CLSIDFromString(wide.c_str(), &guid))) {
        return guid;
    }
    return std::nullopt;
}

struct WindowSearchState {
    HWND visible_window{nullptr};
    HWND fallback_window{nullptr};
};

BOOL CALLBACK FindProcessWindow(HWND window, LPARAM parameter) {
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id != GetCurrentProcessId() || GetAncestor(window, GA_ROOT) != window) {
        return TRUE;
    }

    auto* state = reinterpret_cast<WindowSearchState*>(parameter);
    if (!state->fallback_window) {
        state->fallback_window = window;
    }
    if (IsWindowVisible(window) && GetWindow(window, GW_OWNER) == nullptr) {
        state->visible_window = window;
        return FALSE;
    }
    return TRUE;
}

HWND FindCurrentProcessTopLevelWindow() {
    const HWND foreground = GetForegroundWindow();
    if (foreground) {
        DWORD process_id = 0;
        GetWindowThreadProcessId(foreground, &process_id);
        if (process_id == GetCurrentProcessId()) {
            return GetAncestor(foreground, GA_ROOT);
        }
    }

    WindowSearchState state;
    EnumWindows(FindProcessWindow, reinterpret_cast<LPARAM>(&state));
    return state.visible_window ? state.visible_window : state.fallback_window;
}


class DirectInputPoller {
  public:
    ~DirectInputPoller() {
        // Static destruction. At process exit the connector thread may have been
        // terminated mid-call (possibly holding mutex_), so never lock or wait
        // here; if it could still be running, leak rather than race it.
        if (connector_running_.load()) {
            return;
        }
        for (IDirectInputDevice8W* device : retired_) {
            ReleaseDevice(device);
        }
        for (auto& [_, device] : devices_) {
            ReleaseDevice(device);
        }
        if (direct_input_) {
            direct_input_->Release();
        }
    }

    InputBindingPollResult PollInput(const InputBinding& binding) {
        std::scoped_lock lock(mutex_);
        InputBindingPollResult result;
        result.device_poll_attempted = true;
        const std::optional<DeviceInputPath> input = ParseDeviceInputPath(binding.input_path);
        if (!input.has_value()) {
            SetDiagnostic(result, InputBindingPollStage::ParseInputPath, E_INVALIDARG);
            return result;
        }

        // Several bindings commonly target the same physical device (multiple
        // pivot profiles plus turbo/depth toggles on one HOTAS). A device read
        // is a Poll+GetDeviceState round-trip on the render path, so reuse one
        // state snapshot per device for the duration of a poll tick instead of
        // hitting the hardware once per binding.
        const std::wstring cache_key = NormalizeGuidText(binding.device_guid);
        const auto now = std::chrono::steady_clock::now();
        if (const auto it = state_cache_.find(cache_key); it != state_cache_.end() &&
                                                          now - it->second.read_time < kStateCacheLifetime) {
            result.down = priming_.Filter(cache_key, binding.input_path,
                                          it->second.valid && IsStateDown(*input, it->second.state));
            return result;
        }

        // A background setup failure surfaces once as a real attempt so the
        // caller logs and counts it; later polls read it as deferred below.
        if (unreported_failures_.erase(cache_key) > 0) {
            if (const auto failure = failure_diagnostics_.find(cache_key);
                failure != failure_diagnostics_.end()) {
                result = failure->second;
                result.down = false;
                result.device_poll_attempted = true;
                result.device_retry_deferred = false;
                result.device_retry_delay_ms =
                    retry_backoff_.RetryDelayRemaining(cache_key, now).count();
                return result;
            }
        }

        // A missing HID can make DirectInput setup/reacquisition surprisingly
        // expensive. Without a negative cache, every binding for the same
        // unplugged device retries every 30ms. Return the last inactive
        // diagnostic until this device's reconnect deadline, then permit one
        // new attempt for all of its bindings.
        if (!retry_backoff_.ShouldAttempt(cache_key, now)) {
            if (const auto failure = failure_diagnostics_.find(cache_key);
                failure != failure_diagnostics_.end()) {
                result = failure->second;
            }
            result.down = false;
            result.device_poll_attempted = true;
            result.device_retry_deferred = true;
            result.device_retry_delay_ms =
                retry_backoff_.RetryDelayRemaining(cache_key, now).count();
            return result;
        }

        const auto cache_failure = [&] {
            result.down = false;
            result.device_retry_deferred = false;
            result.device_retry_delay_ms =
                retry_backoff_.RecordFailure(cache_key, InputDeviceRetryBackoff::Clock::now()).count();
            failure_diagnostics_[cache_key] = result;
        };

        const auto device = devices_.find(cache_key);
        if (device == devices_.end()) {
            if (pending_connects_.contains(cache_key)) {
                result.device_connect_pending = true;
                return result;
            }
            if (binding.device_guid.empty() || !ParseGuid(binding.device_guid).has_value()) {
                SetDiagnostic(result, InputBindingPollStage::ParseDeviceGuid, E_INVALIDARG);
                cache_failure();
                return result;
            }
            // CreateDevice, the window lookup, SetCooperativeLevel and Acquire
            // cost 1-6ms per device, and a failing device repeats them at every
            // reconnect deadline (2s at steady state): a periodic frame-time
            // spike on the OpenXR frame thread. The connector does that work;
            // the binding stays inactive until the device is published.
            pending_connects_.emplace(cache_key, binding.device_guid);
            StartConnectorLocked();
            result.device_connect_pending = true;
            return result;
        }

        CachedDeviceState& cached = state_cache_[cache_key];
        cached.read_time = now;
        cached.valid = ReadState(device->second, cached.state, result);
        if (!cached.valid) {
            RetireDeviceLocked(cache_key);
            cache_failure();
            return result;
        }

        retry_backoff_.RecordSuccess(cache_key);
        failure_diagnostics_.erase(cache_key);

        if (result.diagnostic_stage != InputBindingPollStage::None) {
            result.recovered = true;
        }
        result.down = priming_.Filter(cache_key, binding.input_path, IsStateDown(*input, cached.state));
        return result;
    }

    void Drain(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        connector_idle_.wait_for(lock, timeout, [this] { return !connector_running_.load(); });
    }

  private:
    static void SetDiagnostic(InputBindingPollResult& diagnostic,
                              InputBindingPollStage stage,
                              HRESULT result) {
        diagnostic.diagnostic_stage = stage;
        diagnostic.result_code = ResultCode(result);
        diagnostic.reacquire_attempted = false;
        diagnostic.reacquire_result_code = 0;
        diagnostic.retry_attempted = false;
        diagnostic.retry_result_code = 0;
        diagnostic.recovered = false;
    }

    static bool IsStateDown(const DeviceInputPath& input, const DIJOYSTATE2& state) {
        if (input.kind == DeviceInputKind::Button) {
            return input.index < std::size(state.rgbButtons) &&
                   (state.rgbButtons[input.index] & 0x80) != 0;
        }

        return input.index < std::size(state.rgdwPOV) &&
               DirectInputHatDirection(state.rgdwPOV[input.index]) ==
                   std::optional<std::size_t>(input.direction);
    }

    static void ReleaseDevice(IDirectInputDevice8W* device) {
        if (device) {
            device->Unacquire();
            device->Release();
        }
    }

    static IDirectInput8W* CreateDirectInput(InputBindingPollResult& diagnostic) {
        IDirectInput8W* direct_input = nullptr;
        const HRESULT result = DirectInput8Create(
                GetModuleHandleW(nullptr),
                DIRECTINPUT_VERSION,
                IID_IDirectInput8W,
                reinterpret_cast<void**>(&direct_input),
                nullptr);
        if (FAILED(result) || !direct_input) {
            SetDiagnostic(diagnostic, InputBindingPollStage::CreateDirectInput,
                          FAILED(result) ? result : E_POINTER);
            return nullptr;
        }
        return direct_input;
    }

    // Runs on the connector thread, outside mutex_.
    static IDirectInputDevice8W* CreateDevice(IDirectInput8W* direct_input,
                                              std::string_view device_guid_text,
                                              InputBindingPollResult& diagnostic) {
        const std::optional<GUID> device_guid = ParseGuid(device_guid_text);
        if (!device_guid.has_value()) {
            SetDiagnostic(diagnostic, InputBindingPollStage::ParseDeviceGuid, E_INVALIDARG);
            return nullptr;
        }

        IDirectInputDevice8W* device = nullptr;
        const HRESULT create_result = direct_input->CreateDevice(*device_guid, &device, nullptr);
        if (FAILED(create_result) || !device) {
            SetDiagnostic(diagnostic, InputBindingPollStage::CreateDevice,
                          FAILED(create_result) ? create_result : E_POINTER);
            return nullptr;
        }

        const HRESULT format_result = device->SetDataFormat(&c_dfDIJoystick2);
        if (FAILED(format_result)) {
            SetDiagnostic(diagnostic, InputBindingPollStage::SetDataFormat, format_result);
            device->Release();
            return nullptr;
        }

        const HWND cooperative_window = FindCurrentProcessTopLevelWindow();
        diagnostic.cooperative_window = reinterpret_cast<std::uintptr_t>(cooperative_window);
        if (!cooperative_window) {
            SetDiagnostic(diagnostic, InputBindingPollStage::FindTopLevelWindow, E_HANDLE);
            device->Release();
            return nullptr;
        }

        const HRESULT cooperative_result =
            device->SetCooperativeLevel(cooperative_window, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
        if (FAILED(cooperative_result)) {
            SetDiagnostic(diagnostic, InputBindingPollStage::SetCooperativeLevel, cooperative_result);
            device->Release();
            return nullptr;
        }

        // A failed Acquire is not fatal: the frame thread's read reacquires
        // once and retires the device if that still fails.
        device->Acquire();
        return device;
    }

    bool ReadState(IDirectInputDevice8W* device,
                   DIJOYSTATE2& state,
                   InputBindingPollResult& diagnostic) {
        HRESULT poll_result = device->Poll();
        if (FAILED(poll_result)) {
            SetDiagnostic(diagnostic, InputBindingPollStage::Poll, poll_result);
            const HRESULT reacquire_result = device->Acquire();
            diagnostic.reacquire_attempted = true;
            diagnostic.reacquire_result_code = ResultCode(reacquire_result);
            poll_result = device->Poll();
            diagnostic.retry_attempted = true;
            diagnostic.retry_result_code = ResultCode(poll_result);
            if (FAILED(poll_result)) {
                return false;
            }
            diagnostic.recovered = true;
        }

        HRESULT state_result = device->GetDeviceState(sizeof(DIJOYSTATE2), &state);
        if (FAILED(state_result)) {
            SetDiagnostic(diagnostic, InputBindingPollStage::GetDeviceState, state_result);
            const HRESULT reacquire_result = device->Acquire();
            diagnostic.reacquire_attempted = true;
            diagnostic.reacquire_result_code = ResultCode(reacquire_result);
            state_result = device->GetDeviceState(sizeof(DIJOYSTATE2), &state);
            diagnostic.retry_attempted = true;
            diagnostic.retry_result_code = ResultCode(state_result);
            if (FAILED(state_result)) {
                return false;
            }
            diagnostic.recovered = true;
        }

        return true;
    }

    // Unacquire/Release close the HID handle, so they also run on the connector.
    void RetireDeviceLocked(const std::wstring& cache_key) {
        const auto it = devices_.find(cache_key);
        if (it == devices_.end()) {
            return;
        }
        if (it->second) {
            retired_.push_back(it->second);
        }
        devices_.erase(it);
        state_cache_.erase(cache_key);
        StartConnectorLocked();
    }

    void StartConnectorLocked() {
        if (connector_running_.load()) {
            return; // The running connector drains newly queued work before exiting.
        }
        // Short-lived: it exits as soon as the queue is empty. The loader may
        // FreeLibrary the layer after xrDestroyInstance even if Drain timed out
        // (a hung HID), so the thread holds a module reference and drops it
        // with FreeLibraryAndExitThread; it never runs unmapped code.
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                reinterpret_cast<LPCWSTR>(&ConnectorThread), &module)) {
            module = nullptr;
        }
        connector_running_.store(true);
        auto* start = new (std::nothrow) ConnectorStart{this, module};
        const HANDLE thread = start ? CreateThread(nullptr, 0, &ConnectorThread, start, 0, nullptr) : nullptr;
        if (thread) {
            CloseHandle(thread);
            return;
        }
        delete start;
        // Could not start: fail the queued connects onto the reconnect backoff
        // instead of leaving their bindings pending forever.
        if (module) {
            FreeLibrary(module);
        }
        connector_running_.store(false);
        connector_idle_.notify_all();
        for (const auto& [key, guid] : pending_connects_) {
            InputBindingPollResult failure;
            failure.device_poll_attempted = true;
            SetDiagnostic(failure, InputBindingPollStage::CreateDevice, E_OUTOFMEMORY);
            failure.device_retry_delay_ms =
                retry_backoff_.RecordFailure(key, InputDeviceRetryBackoff::Clock::now()).count();
            failure_diagnostics_[key] = failure;
            unreported_failures_.insert(key);
        }
        pending_connects_.clear();
    }

    struct ConnectorStart {
        DirectInputPoller* poller;
        HMODULE module;
    };

    static DWORD WINAPI ConnectorThread(void* parameter) {
        const ConnectorStart start = *static_cast<ConnectorStart*>(parameter);
        delete static_cast<ConnectorStart*>(parameter);
        try {
            start.poller->ConnectorLoop();
        } catch (...) {
            // Input diagnostics must never terminate the host application.
            std::scoped_lock lock(start.poller->mutex_);
            start.poller->connector_running_.store(false);
            start.poller->connector_idle_.notify_all();
        }
        if (start.module) {
            FreeLibraryAndExitThread(start.module, 0);
        }
        return 0;
    }

    void ConnectorLoop() {
        for (;;) {
            std::vector<IDirectInputDevice8W*> retired;
            std::optional<std::pair<std::wstring, std::string>> job;
            IDirectInput8W* direct_input = nullptr;
            {
                std::scoped_lock lock(mutex_);
                retired.swap(retired_);
                if (!pending_connects_.empty()) {
                    job = *pending_connects_.begin();
                } else if (retired.empty()) {
                    connector_running_.store(false);
                    connector_idle_.notify_all();
                    return;
                }
                direct_input = direct_input_;
            }

            for (IDirectInputDevice8W* device : retired) {
                ReleaseDevice(device);
            }
            if (!job.has_value()) {
                continue;
            }

            InputBindingPollResult diagnostic;
            diagnostic.device_poll_attempted = true;
            const bool created_direct_input = direct_input == nullptr;
            if (created_direct_input) {
                direct_input = CreateDirectInput(diagnostic);
            }
            IDirectInputDevice8W* device =
                direct_input ? CreateDevice(direct_input, job->second, diagnostic) : nullptr;

            std::scoped_lock lock(mutex_);
            if (created_direct_input && direct_input) {
                direct_input_ = direct_input; // Only the connector creates it.
            }
            pending_connects_.erase(job->first);
            if (device) {
                devices_[job->first] = device;
                priming_.DeviceConnected(job->first);
                continue;
            }
            diagnostic.down = false;
            diagnostic.device_retry_deferred = false;
            diagnostic.device_retry_delay_ms =
                retry_backoff_.RecordFailure(job->first, InputDeviceRetryBackoff::Clock::now()).count();
            failure_diagnostics_[job->first] = diagnostic;
            unreported_failures_.insert(job->first);
        }
    }

    // Shorter than the callers' poll interval (30ms) so a snapshot never spans
    // two edge-detection ticks, but long enough to collapse all same-tick reads.
    static constexpr std::chrono::milliseconds kStateCacheLifetime{10};

    struct CachedDeviceState {
        DIJOYSTATE2 state{};
        std::chrono::steady_clock::time_point read_time{};
        bool valid{false};
    };

    IDirectInput8W* direct_input_{nullptr};
    std::mutex mutex_;
    std::condition_variable connector_idle_;
    std::atomic<bool> connector_running_{false};
    InputDeviceRetryBackoff retry_backoff_;
    InputConnectPriming priming_;
    std::unordered_map<std::wstring, IDirectInputDevice8W*> devices_;
    std::unordered_map<std::wstring, CachedDeviceState> state_cache_;
    std::unordered_map<std::wstring, InputBindingPollResult> failure_diagnostics_;
    // Device key -> GUID text awaiting background setup.
    std::unordered_map<std::wstring, std::string> pending_connects_;
    std::unordered_set<std::wstring> unreported_failures_;
    std::vector<IDirectInputDevice8W*> retired_;
};

DirectInputPoller& Poller() {
    static DirectInputPoller poller;
    return poller;
}
#endif

InputBindingPollResult PollDeviceBinding(const InputBinding& binding) {
    InputBindingPollResult result;
    if (binding.type != InputBindingType::Device) {
        return result;
    }

#if defined(_WIN32)
    return Poller().PollInput(binding);
#else
    return result;
#endif
}

} // namespace

InputBindingPollResult PollInputBinding(const InputBinding& binding) {
    InputBindingPollResult result;
    if (binding.type == InputBindingType::Keyboard) {
        result.down = IsKeyboardBindingDown(binding);
        return result;
    }

    if (binding.type == InputBindingType::Device) {
        return PollDeviceBinding(binding);
    }

    return result;
}

bool IsInputBindingDown(const InputBinding& binding) {
    return PollInputBinding(binding).down;
}

void DrainInputDeviceWork(std::chrono::milliseconds timeout) {
#if defined(_WIN32)
    Poller().Drain(timeout);
#else
    (void)timeout;
#endif
}

} // namespace depthxr
