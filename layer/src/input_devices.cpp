#include "depthxr/input_devices.h"

#include <algorithm>
#include <cctype>
#include <cwctype>
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
namespace {

#if defined(_WIN32)
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

std::optional<size_t> ParseButtonIndex(std::string_view input_path) {
    constexpr std::string_view prefix = "button-";
    if (!input_path.starts_with(prefix)) {
        return std::nullopt;
    }

    const std::string number(input_path.substr(prefix.size()));
    if (number.empty() || !std::all_of(number.begin(), number.end(), [](unsigned char character) { return std::isdigit(character) != 0; })) {
        return std::nullopt;
    }

    const int one_based_index = std::stoi(number);
    if (one_based_index < 1 || one_based_index > 128) {
        return std::nullopt;
    }

    return static_cast<size_t>(one_based_index - 1);
}

class DirectInputButtonPoller {
  public:
    ~DirectInputButtonPoller() {
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

    bool IsButtonDown(const InputBinding& binding) {
        const std::optional<size_t> button_index = ParseButtonIndex(binding.input_path);
        if (!button_index.has_value()) {
            return false;
        }

        IDirectInputDevice8W* device = GetOrCreateDevice(binding.device_guid);
        if (!device) {
            return false;
        }

        DIJOYSTATE2 state{};
        if (!ReadState(device, state)) {
            DropDevice(binding.device_guid);
            return false;
        }

        return (state.rgbButtons[*button_index] & 0x80) != 0;
    }

  private:
    IDirectInput8W* GetDirectInput() {
        if (direct_input_) {
            return direct_input_;
        }

        if (FAILED(DirectInput8Create(
                GetModuleHandleW(nullptr),
                DIRECTINPUT_VERSION,
                IID_IDirectInput8W,
                reinterpret_cast<void**>(&direct_input_),
                nullptr))) {
            direct_input_ = nullptr;
        }

        return direct_input_;
    }

    IDirectInputDevice8W* GetOrCreateDevice(std::string_view device_guid_text) {
        if (device_guid_text.empty()) {
            return nullptr;
        }

        const std::wstring cache_key = NormalizeGuidText(device_guid_text);
        if (const auto it = devices_.find(cache_key); it != devices_.end()) {
            return it->second;
        }

        IDirectInput8W* direct_input = GetDirectInput();
        const std::optional<GUID> device_guid = ParseGuid(device_guid_text);
        if (!direct_input || !device_guid.has_value()) {
            return nullptr;
        }

        IDirectInputDevice8W* device = nullptr;
        if (FAILED(direct_input->CreateDevice(*device_guid, &device, nullptr)) || !device) {
            return nullptr;
        }

        if (FAILED(device->SetDataFormat(&c_dfDIJoystick2))) {
            device->Release();
            return nullptr;
        }

        device->SetCooperativeLevel(nullptr, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
        device->Acquire();
        devices_[cache_key] = device;
        return device;
    }

    bool ReadState(IDirectInputDevice8W* device, DIJOYSTATE2& state) {
        HRESULT result = device->Poll();
        if (FAILED(result)) {
            device->Acquire();
            result = device->Poll();
        }

        result = device->GetDeviceState(sizeof(DIJOYSTATE2), &state);
        if (FAILED(result)) {
            device->Acquire();
            result = device->GetDeviceState(sizeof(DIJOYSTATE2), &state);
        }

        return SUCCEEDED(result);
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
    }

    IDirectInput8W* direct_input_{nullptr};
    std::unordered_map<std::wstring, IDirectInputDevice8W*> devices_;
};

DirectInputButtonPoller& Poller() {
    static DirectInputButtonPoller poller;
    return poller;
}
#endif

bool IsDeviceBindingDown(const InputBinding& binding) {
    if (binding.type != InputBindingType::Device) {
        return false;
    }

#if defined(_WIN32)
    return Poller().IsButtonDown(binding);
#else
    return false;
#endif
}

} // namespace

bool IsInputBindingDown(const InputBinding& binding) {
    if (binding.type == InputBindingType::Keyboard) {
        return IsKeyboardBindingDown(binding);
    }

    if (binding.type == InputBindingType::Device) {
        return IsDeviceBindingDown(binding);
    }

    return false;
}

} // namespace depthxr
