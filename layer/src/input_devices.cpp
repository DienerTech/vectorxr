#include "depthxr/input_devices.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cwctype>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
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
        for (auto& [_, device] : devices_) {
            if (device) {
                device->Unacquire();
                device->Release();
            }
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
            result.down = it->second.valid && IsStateDown(*input, it->second.state);
            return result;
        }

        IDirectInputDevice8W* device = GetOrCreateDevice(binding.device_guid, result);
        if (!device) {
            return result;
        }

        CachedDeviceState& cached = state_cache_[cache_key];
        cached.read_time = now;
        cached.valid = ReadState(device, cached.state, result);
        if (!cached.valid) {
            DropDevice(binding.device_guid);
            return result;
        }

        if (result.diagnostic_stage != InputBindingPollStage::None) {
            result.recovered = true;
        }
        result.down = IsStateDown(*input, cached.state);
        return result;
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

    IDirectInput8W* GetDirectInput(InputBindingPollResult& diagnostic) {
        if (direct_input_) {
            return direct_input_;
        }

        const HRESULT result = DirectInput8Create(
                GetModuleHandleW(nullptr),
                DIRECTINPUT_VERSION,
                IID_IDirectInput8W,
                reinterpret_cast<void**>(&direct_input_),
                nullptr);
        if (FAILED(result) || !direct_input_) {
            direct_input_ = nullptr;
            SetDiagnostic(diagnostic, InputBindingPollStage::CreateDirectInput,
                          FAILED(result) ? result : E_POINTER);
        }

        return direct_input_;
    }

    IDirectInputDevice8W* GetOrCreateDevice(std::string_view device_guid_text,
                                           InputBindingPollResult& diagnostic) {
        if (device_guid_text.empty()) {
            SetDiagnostic(diagnostic, InputBindingPollStage::ParseDeviceGuid, E_INVALIDARG);
            return nullptr;
        }

        const std::wstring cache_key = NormalizeGuidText(device_guid_text);
        if (const auto it = devices_.find(cache_key); it != devices_.end()) {
            return it->second;
        }

        IDirectInput8W* direct_input = GetDirectInput(diagnostic);
        const std::optional<GUID> device_guid = ParseGuid(device_guid_text);
        if (!direct_input) {
            return nullptr;
        }
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

        const HRESULT acquire_result = device->Acquire();
        if (FAILED(acquire_result)) {
            SetDiagnostic(diagnostic, InputBindingPollStage::Acquire, acquire_result);
        }
        devices_[cache_key] = device;
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

    void DropDevice(std::string_view device_guid_text) {
        const std::wstring cache_key = NormalizeGuidText(device_guid_text);
        const auto it = devices_.find(cache_key);
        if (it == devices_.end()) {
            return;
        }

        if (it->second) {
            it->second->Unacquire();
            it->second->Release();
        }
        devices_.erase(it);
        state_cache_.erase(cache_key);
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
    std::unordered_map<std::wstring, IDirectInputDevice8W*> devices_;
    std::unordered_map<std::wstring, CachedDeviceState> state_cache_;
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

} // namespace depthxr
