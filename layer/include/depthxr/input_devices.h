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

std::optional<DeviceInputPath> ParseDeviceInputPath(std::string_view input_path);
std::optional<std::size_t> DirectInputHatDirection(std::uint32_t value);

bool IsInputBindingDown(const InputBinding& binding);

} // namespace depthxr
