#pragma once

#include "depthxr/settings.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

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
    InputBindingPollStage diagnostic_stage{InputBindingPollStage::None};
    std::int64_t result_code{0};
    bool reacquire_attempted{false};
    std::int64_t reacquire_result_code{0};
    bool retry_attempted{false};
    std::int64_t retry_result_code{0};
    bool recovered{false};
    std::uintptr_t cooperative_window{0};
};

std::optional<DeviceInputPath> ParseDeviceInputPath(std::string_view input_path);
std::optional<std::size_t> DirectInputHatDirection(std::uint32_t value);
const char* ToString(InputBindingPollStage stage);
const char* DirectInputResultName(std::int64_t result_code);

InputBindingPollResult PollInputBinding(const InputBinding& binding);
bool IsInputBindingDown(const InputBinding& binding);

} // namespace depthxr
