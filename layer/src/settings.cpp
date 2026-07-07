#include "depthxr/settings.h"

#include <algorithm>
#include <cctype>

namespace depthxr {
namespace {

std::string TrimWhitespace(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }

    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }

    return value.substr(begin, end - begin);
}

std::string NormalizeValue(const std::string& value) {
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return normalized;
}

} // namespace

const char* ToString(LogLevel level) {
    switch (level) {
    case LogLevel::Error:
        return "error";
    case LogLevel::Info:
        return "info";
    case LogLevel::Debug:
        return "debug";
    default:
        return "info";
    }
}

std::optional<LogLevel> ParseLogLevel(const std::string& value) {
    const std::string normalized = NormalizeValue(value);

    if (normalized == "none" || normalized == "off" || normalized == "error") {
        return LogLevel::Info;
    }
    if (normalized == "info") {
        return LogLevel::Info;
    }
    if (normalized == "debug") {
        return LogLevel::Debug;
    }

    return std::nullopt;
}

const char* ToString(ActivationMode mode) {
    switch (mode) {
    case ActivationMode::Toggle:
        return "toggle";
    case ActivationMode::Hold:
        return "hold";
    case ActivationMode::AlwaysOn:
        return "alwaysOn";
    default:
        return "toggle";
    }
}

std::optional<ActivationMode> ParseActivationMode(const std::string& value) {
    const std::string normalized = NormalizeValue(value);

    if (normalized == "toggle") {
        return ActivationMode::Toggle;
    }
    if (normalized == "hold") {
        return ActivationMode::Hold;
    }
    if (normalized == "alwayson" || normalized == "always_on" || normalized == "always-on") {
        return ActivationMode::AlwaysOn;
    }

    return std::nullopt;
}

const char* ToString(PivotResponseMode mode) {
    switch (mode) {
    case PivotResponseMode::Continuous:
        return "continuous";
    case PivotResponseMode::Stepped:
        return "stepped";
    default:
        return "continuous";
    }
}

std::optional<PivotResponseMode> ParsePivotResponseMode(const std::string& value) {
    const std::string normalized = NormalizeValue(value);

    if (normalized == "continuous") {
        return PivotResponseMode::Continuous;
    }
    if (normalized == "stepped") {
        return PivotResponseMode::Stepped;
    }

    return std::nullopt;
}

std::optional<std::string> ParseActivationKey(const std::string& value) {
    const std::string trimmed = TrimWhitespace(value);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    if (trimmed.size() == 1) {
        const unsigned char character = static_cast<unsigned char>(trimmed[0]);
        if (std::isalpha(character)) {
            return std::string(1, static_cast<char>(std::toupper(character)));
        }
        if (std::isdigit(character)) {
            return trimmed;
        }
    }

    const std::string normalized = NormalizeValue(trimmed);
    if (normalized == "space") {
        return std::string("Space");
    }

    if (normalized == "ctrl" || normalized == "control") {
        return std::string("Ctrl");
    }
    if (normalized == "alt") {
        return std::string("Alt");
    }
    if (normalized == "shift") {
        return std::string("Shift");
    }

    if (normalized.size() >= 2 && normalized[0] == 'f') {
        bool digits_only = true;
        for (size_t index = 1; index < normalized.size(); ++index) {
            if (!std::isdigit(static_cast<unsigned char>(normalized[index]))) {
                digits_only = false;
                break;
            }
        }

        if (digits_only) {
            try {
                const int function_key = std::stoi(normalized.substr(1));
                if (function_key >= 1 && function_key <= 12) {
                    return std::string("F") + std::to_string(function_key);
                }
            } catch (...) {
                return std::nullopt;
            }
        }
    }

    return std::nullopt;
}

const char* ToString(InputBindingType type) {
    switch (type) {
    case InputBindingType::None:
        return "none";
    case InputBindingType::Keyboard:
        return "keyboard";
    case InputBindingType::Device:
        return "device";
    default:
        return "keyboard";
    }
}

std::optional<InputBindingType> ParseInputBindingType(const std::string& value) {
    const std::string normalized = NormalizeValue(value);

    if (normalized == "none") {
        return InputBindingType::None;
    }
    if (normalized == "keyboard") {
        return InputBindingType::Keyboard;
    }
    if (normalized == "device") {
        return InputBindingType::Device;
    }

    return std::nullopt;
}

const char* ToString(QuadViewsTrackingMode mode) {
    switch (mode) {
    case QuadViewsTrackingMode::Head:
        return "head";
    case QuadViewsTrackingMode::Eye:
        return "eye";
    default:
        return "head";
    }
}

std::optional<QuadViewsTrackingMode> ParseQuadViewsTrackingMode(const std::string& value) {
    const std::string normalized = NormalizeValue(value);

    if (normalized == "head") {
        return QuadViewsTrackingMode::Head;
    }
    if (normalized == "eye") {
        return QuadViewsTrackingMode::Eye;
    }

    return std::nullopt;
}

const char* ToString(ProfileMode mode) {
    switch (mode) {
    case ProfileMode::Custom:
        return "custom";
    case ProfileMode::Disable:
        return "disable";
    default:
        return "custom";
    }
}

std::optional<ProfileMode> ParseProfileMode(const std::string& value) {
    const std::string normalized = NormalizeValue(value);

    if (normalized == "custom") {
        return ProfileMode::Custom;
    }
    if (normalized == "disable") {
        return ProfileMode::Disable;
    }

    return std::nullopt;
}

const char* ToString(TurboPacingMode mode) {
    switch (mode) {
    case TurboPacingMode::kAsync:
        return "async";
    case TurboPacingMode::kSequenced:
        return "sequenced";
    case TurboPacingMode::kUnsupported:
        return "unsupported";
    default:
        return "async";
    }
}

std::optional<TurboPacingMode> ParseTurboPacingMode(const std::string& value) {
    const std::string normalized = NormalizeValue(value);

    if (normalized == "async") {
        return TurboPacingMode::kAsync;
    }
    if (normalized == "sequenced") {
        return TurboPacingMode::kSequenced;
    }
    if (normalized == "unsupported") {
        return TurboPacingMode::kUnsupported;
    }

    return std::nullopt;
}

const char* ToString(TurboPacingSetting setting) {
    switch (setting) {
    case TurboPacingSetting::kAuto:
        return "auto";
    case TurboPacingSetting::kAsync:
        return "async";
    case TurboPacingSetting::kSequenced:
        return "sequenced";
    default:
        return "auto";
    }
}

std::optional<TurboPacingSetting> ParseTurboPacingSetting(const std::string& value) {
    const std::string normalized = NormalizeValue(value);

    if (normalized == "auto") {
        return TurboPacingSetting::kAuto;
    }
    if (normalized == "async") {
        return TurboPacingSetting::kAsync;
    }
    if (normalized == "sequenced") {
        return TurboPacingSetting::kSequenced;
    }

    return std::nullopt;
}

} // namespace depthxr
