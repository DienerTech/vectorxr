#include "depthxr/config_parser.h"

#include <cctype>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <variant>
#include <vector>

namespace depthxr {
namespace {

class JsonValue {
  public:
    using Object = std::map<std::string, JsonValue>;
    using Array = std::vector<JsonValue>;
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, Object, Array>;

    JsonValue() : storage_(nullptr) {}
    explicit JsonValue(std::nullptr_t) : storage_(nullptr) {}
    explicit JsonValue(bool value) : storage_(value) {}
    explicit JsonValue(double value) : storage_(value) {}
    explicit JsonValue(std::string value) : storage_(std::move(value)) {}
    explicit JsonValue(Object value) : storage_(std::move(value)) {}
    explicit JsonValue(Array value) : storage_(std::move(value)) {}

    bool IsBool() const { return std::holds_alternative<bool>(storage_); }
    bool IsNumber() const { return std::holds_alternative<double>(storage_); }
    bool IsString() const { return std::holds_alternative<std::string>(storage_); }
    bool IsObject() const { return std::holds_alternative<Object>(storage_); }
    bool IsArray() const { return std::holds_alternative<Array>(storage_); }

    bool AsBool() const { return std::get<bool>(storage_); }
    double AsNumber() const { return std::get<double>(storage_); }
    const std::string& AsString() const { return std::get<std::string>(storage_); }
    const Object& AsObject() const { return std::get<Object>(storage_); }
    const Array& AsArray() const { return std::get<Array>(storage_); }

  private:
    Storage storage_;
};

class JsonParser {
  public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    JsonValue Parse() {
        SkipWhitespace();
        JsonValue value = ParseValue();
        SkipWhitespace();

        if (!ok_) {
            return JsonValue();
        }

        if (position_ != input_.size()) {
            Fail("Unexpected trailing characters");
        }

        return value;
    }

    bool ok() const { return ok_; }
    const std::string& error() const { return error_; }

  private:
    JsonValue ParseValue() {
        SkipWhitespace();
        if (position_ >= input_.size()) {
            Fail("Unexpected end of input");
            return JsonValue();
        }

        const char c = input_[position_];
        if (c == '{') {
            return ParseObject();
        }
        if (c == '[') {
            return ParseArray();
        }
        if (c == '"') {
            return JsonValue(ParseString());
        }
        if (c == 't' || c == 'f') {
            return JsonValue(ParseBool());
        }
        if (c == 'n') {
            ParseNull();
            return JsonValue(nullptr);
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            return JsonValue(ParseNumber());
        }

        Fail("Unexpected token");
        return JsonValue();
    }

    JsonValue ParseObject() {
        Expect('{');
        JsonValue::Object object;
        SkipWhitespace();

        if (Match('}')) {
            return JsonValue(std::move(object));
        }

        while (ok_) {
            SkipWhitespace();
            if (!Match('"')) {
                Fail("Expected string key");
                return JsonValue();
            }
            const std::string key = ParseStringBody();

            SkipWhitespace();
            Expect(':');
            SkipWhitespace();
            object.emplace(key, ParseValue());
            SkipWhitespace();

            if (Match('}')) {
                break;
            }
            Expect(',');
        }

        return JsonValue(std::move(object));
    }

    JsonValue ParseArray() {
        Expect('[');
        JsonValue::Array array;
        SkipWhitespace();

        if (Match(']')) {
            return JsonValue(std::move(array));
        }

        while (ok_) {
            array.push_back(ParseValue());
            SkipWhitespace();

            if (Match(']')) {
                break;
            }
            Expect(',');
        }

        return JsonValue(std::move(array));
    }

    std::string ParseString() {
        Expect('"');
        return ParseStringBody();
    }

    std::string ParseStringBody() {
        std::string result;

        while (ok_ && position_ < input_.size()) {
            const char c = input_[position_++];
            if (c == '"') {
                return result;
            }

            if (c == '\\') {
                if (position_ >= input_.size()) {
                    Fail("Invalid escape sequence");
                    return {};
                }

                const char escaped = input_[position_++];
                switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    result.push_back(escaped);
                    break;
                case 'b':
                    result.push_back('\b');
                    break;
                case 'f':
                    result.push_back('\f');
                    break;
                case 'n':
                    result.push_back('\n');
                    break;
                case 'r':
                    result.push_back('\r');
                    break;
                case 't':
                    result.push_back('\t');
                    break;
                case 'u':
                    if (position_ + 4 > input_.size()) {
                        Fail("Invalid unicode escape");
                        return {};
                    }
                    position_ += 4;
                    result.push_back('?');
                    break;
                default:
                    Fail("Unsupported escape sequence");
                    return {};
                }
                continue;
            }

            result.push_back(c);
        }

        Fail("Unterminated string");
        return {};
    }

    bool ParseBool() {
        if (input_.substr(position_, 4) == "true") {
            position_ += 4;
            return true;
        }
        if (input_.substr(position_, 5) == "false") {
            position_ += 5;
            return false;
        }

        Fail("Invalid boolean");
        return false;
    }

    void ParseNull() {
        if (input_.substr(position_, 4) == "null") {
            position_ += 4;
            return;
        }

        Fail("Invalid null");
    }

    double ParseNumber() {
        const size_t start = position_;

        if (input_[position_] == '-') {
            ++position_;
        }

        while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }

        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                ++position_;
            }
        }

        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                ++position_;
            }
        }

        try {
            return std::stod(std::string(input_.substr(start, position_ - start)));
        } catch (const std::exception&) {
            Fail("Invalid number");
            return 0.0;
        }
    }

    void SkipWhitespace() {
        while (position_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
    }

    bool Match(char expected) {
        if (position_ < input_.size() && input_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void Expect(char expected) {
        if (!Match(expected)) {
            std::ostringstream stream;
            stream << "Expected '" << expected << "'";
            Fail(stream.str());
        }
    }

    void Fail(const std::string& message) {
        if (!ok_) {
            return;
        }
        ok_ = false;
        std::ostringstream stream;
        stream << message << " at offset " << position_;
        error_ = stream.str();
    }

    std::string_view input_;
    size_t position_{0};
    bool ok_{true};
    std::string error_;
};

const JsonValue::Object* RequireObject(const JsonValue& value, const std::string& field, std::string& error) {
    if (!value.IsObject()) {
        error = field + " must be an object";
        return nullptr;
    }
    return &value.AsObject();
}

const JsonValue::Array* RequireArray(const JsonValue& value, const std::string& field, std::string& error) {
    if (!value.IsArray()) {
        error = field + " must be an array";
        return nullptr;
    }
    return &value.AsArray();
}

bool ReadRequiredBool(const JsonValue::Object& object, const std::string& key, bool& out, std::string& error) {
    const auto it = object.find(key);
    if (it == object.end()) {
        error = "Missing required field: " + key;
        return false;
    }
    if (!it->second.IsBool()) {
        error = key + " must be a boolean";
        return false;
    }
    out = it->second.AsBool();
    return true;
}

bool ReadOptionalBool(const JsonValue::Object& object, const std::string& key, std::optional<bool>& out, std::string& error) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return true;
    }
    if (!it->second.IsBool()) {
        error = key + " must be a boolean";
        return false;
    }
    out = it->second.AsBool();
    return true;
}

bool ReadRequiredNumber(const JsonValue::Object& object, const std::string& key, double& out, std::string& error) {
    const auto it = object.find(key);
    if (it == object.end()) {
        error = "Missing required field: " + key;
        return false;
    }
    if (!it->second.IsNumber()) {
        error = key + " must be a number";
        return false;
    }
    out = it->second.AsNumber();
    return true;
}

bool ReadOptionalNumber(const JsonValue::Object& object, const std::string& key, std::optional<double>& out, std::string& error) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return true;
    }
    if (!it->second.IsNumber()) {
        error = key + " must be a number";
        return false;
    }
    out = it->second.AsNumber();
    return true;
}

bool ReadOptionalInt(const JsonValue::Object& object, const std::string& key, std::optional<int>& out, std::string& error) {
    std::optional<double> number;
    if (!ReadOptionalNumber(object, key, number, error)) {
        return false;
    }
    if (!number.has_value()) {
        return true;
    }
    out = static_cast<int>(*number);
    return true;
}

bool ReadRequiredString(const JsonValue::Object& object, const std::string& key, std::string& out, std::string& error) {
    const auto it = object.find(key);
    if (it == object.end()) {
        error = "Missing required field: " + key;
        return false;
    }
    if (!it->second.IsString()) {
        error = key + " must be a string";
        return false;
    }
    out = it->second.AsString();
    return true;
}

bool ParseStringArray(const JsonValue& value, const std::string& field, std::vector<std::string>& out, std::string& error) {
    const JsonValue::Array* array = RequireArray(value, field, error);
    if (!array) {
        return false;
    }

    for (const JsonValue& item : *array) {
        if (!item.IsString()) {
            error = field + " items must be strings";
            return false;
        }
        out.push_back(item.AsString());
    }

    return true;
}

bool ReadOptionalString(const JsonValue::Object& object, const std::string& key, std::optional<std::string>& out, std::string& error) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return true;
    }
    if (!it->second.IsString()) {
        error = key + " must be a string";
        return false;
    }
    out = it->second.AsString();
    return true;
}

bool ReadOptionalLogLevel(const JsonValue::Object& object,
                          const std::string& key,
                          std::optional<LogLevel>& out,
                          std::string& error) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return true;
    }
    if (!it->second.IsString()) {
        error = key + " must be a string";
        return false;
    }

    out = ParseLogLevel(it->second.AsString());
    if (!out) {
        error = key + " must be one of: none, info, debug";
        return false;
    }
    return true;
}

bool ReadOptionalActivationMode(const JsonValue::Object& object,
                                const std::string& key,
                                std::optional<ActivationMode>& out,
                                std::string& error) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return true;
    }
    if (!it->second.IsString()) {
        error = key + " must be a string";
        return false;
    }

    out = ParseActivationMode(it->second.AsString());
    if (!out) {
        error = key + " must be one of: toggle, hold";
        return false;
    }
    return true;
}

bool ReadOptionalActivationKey(const JsonValue::Object& object,
                               const std::string& key,
                               std::optional<std::string>& out,
                               std::string& error) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return true;
    }
    if (!it->second.IsString()) {
        error = key + " must be a string";
        return false;
    }

    out = ParseActivationKey(it->second.AsString());
    if (!out) {
        error = key + " must be one of: F1-F12, A-Z, 0-9, Space";
        return false;
    }
    return true;
}

bool CheckAllowedKeys(const JsonValue::Object& object,
                      const std::unordered_set<std::string>& allowed,
                      std::string& error);

bool ParseInputBinding(const JsonValue& value, InputBinding& out, std::string& error) {
    const JsonValue::Object* object = RequireObject(value, "inputBinding", error);
    if (!object) {
        return false;
    }

    const auto type_it = object->find("type");
    if (type_it == object->end() || !type_it->second.IsString()) {
        error = "inputBinding.type must be a string";
        return false;
    }

    const std::optional<InputBindingType> type = ParseInputBindingType(type_it->second.AsString());
    if (!type.has_value()) {
        error = "inputBinding.type must be one of: none, keyboard, device";
        return false;
    }

    out = InputBinding{};
    out.type = *type;
    if (out.type == InputBindingType::None) {
        return true;
    }

    if (out.type == InputBindingType::Keyboard) {
        static const std::unordered_set<std::string> allowed = {"type", "chord"};
        if (!CheckAllowedKeys(*object, allowed, error)) {
            return false;
        }

        const auto chord_it = object->find("chord");
        if (chord_it == object->end()) {
            error = "Missing required field: inputBinding.chord";
            return false;
        }

        std::vector<std::string> raw_chord;
        if (!ParseStringArray(chord_it->second, "inputBinding.chord", raw_chord, error)) {
            return false;
        }

        out.chord.clear();
        for (const std::string& key : raw_chord) {
            const std::optional<std::string> normalized = ParseActivationKey(key);
            if (!normalized.has_value()) {
                error = "inputBinding.chord contains unsupported key: " + key;
                return false;
            }
            out.chord.push_back(*normalized);
        }

        if (out.chord.empty()) {
            error = "inputBinding.chord must include at least one key";
            return false;
        }

        return true;
    }

    static const std::unordered_set<std::string> allowed = {"type", "deviceGuid", "inputPath", "productGuid", "deviceName", "inputLabel"};
    if (!CheckAllowedKeys(*object, allowed, error)) {
        return false;
    }

    if (!ReadRequiredString(*object, "deviceGuid", out.device_guid, error) ||
        !ReadRequiredString(*object, "inputPath", out.input_path, error)) {
        return false;
    }

    if (const auto product_guid_it = object->find("productGuid"); product_guid_it != object->end()) {
        if (!product_guid_it->second.IsString()) {
            error = "productGuid must be a string";
            return false;
        }
        out.product_guid = product_guid_it->second.AsString();
    }

    if (const auto device_name_it = object->find("deviceName"); device_name_it != object->end()) {
        if (!device_name_it->second.IsString()) {
            error = "deviceName must be a string";
            return false;
        }
        out.device_name = device_name_it->second.AsString();
    }

    if (const auto input_label_it = object->find("inputLabel"); input_label_it != object->end()) {
        if (!input_label_it->second.IsString()) {
            error = "inputLabel must be a string";
            return false;
        }
        out.input_label = input_label_it->second.AsString();
    }

    return true;
}

bool CheckAllowedKeys(const JsonValue::Object& object,
                      const std::unordered_set<std::string>& allowed,
                      std::string& error) {
    for (const auto& [key, _] : object) {
        if (!allowed.contains(key)) {
            error = "Unknown field: " + key;
            return false;
        }
    }
    return true;
}

bool ParseCoreSettings(const JsonValue::Object& object, CoreSettings& out, std::string& error) {
    static const std::unordered_set<std::string> allowed = {
        "enabled",
        "logLevel",
        "logRetentionFiles",
    };

    if (!CheckAllowedKeys(object, allowed, error)) {
        return false;
    }

    std::optional<bool> enabled;
    std::optional<LogLevel> log_level;
    std::optional<int> log_retention_files;

    if (!ReadOptionalBool(object, "enabled", enabled, error) ||
        !ReadOptionalLogLevel(object, "logLevel", log_level, error) ||
        !ReadOptionalInt(object, "logRetentionFiles", log_retention_files, error)) {
        return false;
    }

    if (enabled.has_value()) {
        out.enabled = *enabled;
    }
    if (log_level.has_value()) {
        out.log_level = *log_level;
    }
    if (log_retention_files.has_value()) {
        out.log_retention_files = *log_retention_files;
    }

    return true;
}

bool ParseApplication(const JsonValue& value, RegisteredApplication& out, std::string& error) {
    const JsonValue::Object* object = RequireObject(value, "application", error);
    if (!object) {
        return false;
    }

    static const std::unordered_set<std::string> allowed = {
        "id",
        "name",
        "enabled",
        "match",
    };

    if (!CheckAllowedKeys(*object, allowed, error)) {
        return false;
    }

    if (!ReadRequiredString(*object, "id", out.id, error) ||
        !ReadRequiredString(*object, "name", out.name, error) ||
        !ReadRequiredBool(*object, "enabled", out.enabled, error)) {
        return false;
    }

    const auto match_it = object->find("match");
    if (match_it == object->end()) {
        error = "Missing required field: match";
        return false;
    }

    const JsonValue::Object* match_object = RequireObject(match_it->second, "match", error);
    if (!match_object) {
        return false;
    }

    static const std::unordered_set<std::string> allowed_match = {"exe"};
    if (!CheckAllowedKeys(*match_object, allowed_match, error)) {
        return false;
    }

    return ReadRequiredString(*match_object, "exe", out.match.exe_name, error);
}

bool ParseDepthDefaults(const JsonValue::Object& object, DepthXrResolvedSettings& out, std::string& error) {
    static const std::unordered_set<std::string> allowed = {
        "stereoBoostEnabled",
        "convergenceEnabled",
        "stereoBoost",
        "convergence",
    };

    if (!CheckAllowedKeys(object, allowed, error)) {
        return false;
    }

    std::optional<bool> stereo_boost_enabled;
    std::optional<bool> convergence_enabled;
    std::optional<double> stereo_boost;
    std::optional<double> convergence;

    if (!ReadOptionalBool(object, "stereoBoostEnabled", stereo_boost_enabled, error) ||
        !ReadOptionalBool(object, "convergenceEnabled", convergence_enabled, error) ||
        !ReadOptionalNumber(object, "stereoBoost", stereo_boost, error) ||
        !ReadOptionalNumber(object, "convergence", convergence, error)) {
        return false;
    }

    if (stereo_boost_enabled.has_value()) {
        out.stereo_boost_enabled = *stereo_boost_enabled;
    }
    if (convergence_enabled.has_value()) {
        out.convergence_enabled = *convergence_enabled;
    }
    if (stereo_boost.has_value()) {
        out.stereo_boost = *stereo_boost;
    }
    if (convergence.has_value()) {
        out.convergence = *convergence;
    }

    return true;
}

bool ParseDepthBindings(const JsonValue::Object& object, DepthXrBindings& out, std::string& error) {
    static const std::unordered_set<std::string> allowed = {
        "toggleEnabled",
    };

    if (!CheckAllowedKeys(object, allowed, error)) {
        return false;
    }

    const auto toggle_it = object.find("toggleEnabled");
    if (toggle_it == object.end()) {
        error = "Missing required field: toggleEnabled";
        return false;
    }

    return ParseInputBinding(toggle_it->second, out.toggle_enabled, error);
}

bool ParseDepthProfileSettings(const JsonValue::Object& object, DepthXrSettingsOverride& out, std::string& error) {
    static const std::unordered_set<std::string> allowed = {
        "stereoBoostEnabled",
        "convergenceEnabled",
        "stereoBoost",
        "convergence",
    };

    if (!CheckAllowedKeys(object, allowed, error)) {
        return false;
    }

    return ReadOptionalBool(object, "stereoBoostEnabled", out.stereo_boost_enabled, error) &&
           ReadOptionalBool(object, "convergenceEnabled", out.convergence_enabled, error) &&
           ReadOptionalNumber(object, "stereoBoost", out.stereo_boost, error) &&
           ReadOptionalNumber(object, "convergence", out.convergence, error);
}

bool ParseDepthProfile(const JsonValue& value, DepthXrProfile& out, std::string& error) {
    const JsonValue::Object* object = RequireObject(value, "profile", error);
    if (!object) {
        return false;
    }

    static const std::unordered_set<std::string> allowed = {
        "name",
        "enabled",
        "applicationIds",
        "settings",
    };

    if (!CheckAllowedKeys(*object, allowed, error)) {
        return false;
    }

    const auto application_ids_it = object->find("applicationIds");
    if (application_ids_it == object->end()) {
        error = "Missing required field: applicationIds";
        return false;
    }
    if (!ParseStringArray(application_ids_it->second, "applicationIds", out.application_ids, error)) {
        return false;
    }

    std::optional<std::string> name;
    std::optional<bool> enabled;
    if (!ReadOptionalString(*object, "name", name, error) ||
        !ReadOptionalBool(*object, "enabled", enabled, error)) {
        return false;
    }

    out.name = name.value_or("New Profile");
    out.enabled = enabled.value_or(true);

    const auto settings_it = object->find("settings");
    if (settings_it != object->end()) {
        const JsonValue::Object* settings_object = RequireObject(settings_it->second, "settings", error);
        if (!settings_object || !ParseDepthProfileSettings(*settings_object, out.settings, error)) {
            return false;
        }
    }

    return true;
}

bool ParseDepthModule(const JsonValue::Object& object,
                      DepthXrModuleConfig& out,
                      std::string& error) {
    static const std::unordered_set<std::string> allowed = {
        "enabled",
        "defaults",
        "bindings",
        "profiles",
    };

    if (!CheckAllowedKeys(object, allowed, error)) {
        return false;
    }

    std::optional<bool> enabled;
    if (!ReadOptionalBool(object, "enabled", enabled, error)) {
        return false;
    }
    if (enabled.has_value()) {
        out.enabled = *enabled;
        out.defaults.enabled = *enabled;
    }

    const auto defaults_it = object.find("defaults");
    if (defaults_it != object.end()) {
        const JsonValue::Object* defaults_object = RequireObject(defaults_it->second, "defaults", error);
        if (!defaults_object || !ParseDepthDefaults(*defaults_object, out.defaults, error)) {
            return false;
        }
    }

    const auto bindings_it = object.find("bindings");
    if (bindings_it != object.end()) {
        const JsonValue::Object* bindings_object = RequireObject(bindings_it->second, "bindings", error);
        if (!bindings_object || !ParseDepthBindings(*bindings_object, out.bindings, error)) {
            return false;
        }
    }

    const auto profiles_it = object.find("profiles");
    if (profiles_it != object.end()) {
        const JsonValue::Array* profiles = RequireArray(profiles_it->second, "profiles", error);
        if (!profiles) {
            return false;
        }

        for (const JsonValue& value : *profiles) {
            DepthXrProfile profile;
            if (!ParseDepthProfile(value, profile, error)) {
                return false;
            }
            out.profiles.push_back(std::move(profile));
        }
    }

    return true;
}

bool ParsePivotSettings(const JsonValue::Object& object, PivotXrSettings& out, std::string& error) {
    static const std::unordered_set<std::string> allowed = {
        "rotationMultiplier",
        "smoothing",
        "deadzoneDegrees",
        "maxExtraYawDegrees",
        "pitchRotationMultiplier",
        "pitchSmoothing",
        "pitchDeadzoneDegrees",
        "maxExtraPitchDegrees",
    };

    if (!CheckAllowedKeys(object, allowed, error)) {
        return false;
    }

    std::optional<double> rotation_multiplier;
    std::optional<double> smoothing;
    std::optional<double> deadzone_degrees;
    std::optional<double> max_extra_yaw_degrees;
    std::optional<double> pitch_rotation_multiplier;
    std::optional<double> pitch_smoothing;
    std::optional<double> pitch_deadzone_degrees;
    std::optional<double> max_extra_pitch_degrees;

    if (!ReadOptionalNumber(object, "rotationMultiplier", rotation_multiplier, error) ||
        !ReadOptionalNumber(object, "smoothing", smoothing, error) ||
        !ReadOptionalNumber(object, "deadzoneDegrees", deadzone_degrees, error) ||
        !ReadOptionalNumber(object, "maxExtraYawDegrees", max_extra_yaw_degrees, error) ||
        !ReadOptionalNumber(object, "pitchRotationMultiplier", pitch_rotation_multiplier, error) ||
        !ReadOptionalNumber(object, "pitchSmoothing", pitch_smoothing, error) ||
        !ReadOptionalNumber(object, "pitchDeadzoneDegrees", pitch_deadzone_degrees, error) ||
        !ReadOptionalNumber(object, "maxExtraPitchDegrees", max_extra_pitch_degrees, error)) {
        return false;
    }

    if (rotation_multiplier.has_value()) {
        out.yaw_rotation_multiplier = *rotation_multiplier;
    }
    if (smoothing.has_value()) {
        out.yaw_smoothing = *smoothing;
    }
    if (deadzone_degrees.has_value()) {
        out.yaw_deadzone_degrees = *deadzone_degrees;
    }
    if (max_extra_yaw_degrees.has_value()) {
        out.yaw_max_extra_degrees = *max_extra_yaw_degrees;
    }
    if (pitch_rotation_multiplier.has_value()) {
        out.pitch_rotation_multiplier = *pitch_rotation_multiplier;
    }
    if (pitch_smoothing.has_value()) {
        out.pitch_smoothing = *pitch_smoothing;
    }
    if (pitch_deadzone_degrees.has_value()) {
        out.pitch_deadzone_degrees = *pitch_deadzone_degrees;
    }
    if (max_extra_pitch_degrees.has_value()) {
        out.pitch_max_extra_degrees = *max_extra_pitch_degrees;
    }

    return true;
}

bool ParsePivotProfile(const JsonValue& value, PivotXrProfile& out, std::string& error) {
    const JsonValue::Object* object = RequireObject(value, "pivotProfile", error);
    if (!object) {
        return false;
    }

    static const std::unordered_set<std::string> allowed = {
        "name",
        "enabled",
        "applicationIds",
        "activationMode",
        "activationBinding",
        "settings",
    };

    if (!CheckAllowedKeys(*object, allowed, error)) {
        return false;
    }

    const auto application_ids_it = object->find("applicationIds");
    if (application_ids_it == object->end()) {
        error = "Missing required field: pivotProfile.applicationIds";
        return false;
    }
    if (!ParseStringArray(application_ids_it->second, "applicationIds", out.application_ids, error)) {
        return false;
    }

    std::optional<std::string> name;
    std::optional<bool> enabled;
    std::optional<ActivationMode> activation_mode;

    if (!ReadOptionalString(*object, "name", name, error) ||
        !ReadOptionalBool(*object, "enabled", enabled, error) ||
        !ReadOptionalActivationMode(*object, "activationMode", activation_mode, error)) {
        return false;
    }

    out.name = name.value_or("New Profile");
    out.enabled = enabled.value_or(true);
    if (activation_mode.has_value()) {
        out.activation_mode = *activation_mode;
    }

    const auto binding_it = object->find("activationBinding");
    if (binding_it != object->end() && !ParseInputBinding(binding_it->second, out.activation_binding, error)) {
        return false;
    }

    const auto settings_it = object->find("settings");
    if (settings_it != object->end()) {
        const JsonValue::Object* settings_object = RequireObject(settings_it->second, "pivotProfile.settings", error);
        if (!settings_object || !ParsePivotSettings(*settings_object, out.settings, error)) {
            return false;
        }
    }

    return true;
}

bool ParsePivotModule(const JsonValue::Object& object, PivotXrModuleConfig& out, std::string& error) {
    static const std::unordered_set<std::string> allowed = {
        "enabled",
        "defaults",
        "activationMode",
        "activationBinding",
        "profiles",
    };

    if (!CheckAllowedKeys(object, allowed, error)) {
        return false;
    }

    std::optional<bool> enabled;
    std::optional<ActivationMode> activation_mode;
    if (!ReadOptionalBool(object, "enabled", enabled, error) ||
        !ReadOptionalActivationMode(object, "activationMode", activation_mode, error)) {
        return false;
    }
    if (enabled.has_value()) {
        out.enabled = *enabled;
    }
    if (activation_mode.has_value()) {
        out.activation_mode = *activation_mode;
    }

    const auto defaults_it = object.find("defaults");
    if (defaults_it != object.end()) {
        const JsonValue::Object* defaults_object = RequireObject(defaults_it->second, "pivotxr.defaults", error);
        if (!defaults_object || !ParsePivotSettings(*defaults_object, out.defaults, error)) {
            return false;
        }
    }

    const auto activation_binding_it = object.find("activationBinding");
    if (activation_binding_it != object.end() && !ParseInputBinding(activation_binding_it->second, out.activation_binding, error)) {
        return false;
    }

    const auto profiles_it = object.find("profiles");
    if (profiles_it != object.end()) {
        const JsonValue::Array* profiles = RequireArray(profiles_it->second, "pivotxr.profiles", error);
        if (!profiles) {
            return false;
        }

        for (const JsonValue& profile_value : *profiles) {
            PivotXrProfile profile;
            if (!ParsePivotProfile(profile_value, profile, error)) {
                return false;
            }
            out.profiles.push_back(std::move(profile));
        }
    }

    return true;
}

bool ParseVectorDocument(const JsonValue::Object& root_object, ConfigDocument& out, std::string& error) {
    static const std::unordered_set<std::string> allowed_root = {"version", "core", "applications", "modules"};
    if (!CheckAllowedKeys(root_object, allowed_root, error)) {
        return false;
    }

    const auto core_it = root_object.find("core");
    if (core_it == root_object.end()) {
        error = "Missing required field: core";
        return false;
    }
    const JsonValue::Object* core_object = RequireObject(core_it->second, "core", error);
    if (!core_object || !ParseCoreSettings(*core_object, out.core, error)) {
        return false;
    }

    const auto applications_it = root_object.find("applications");
    if (applications_it == root_object.end()) {
        error = "Missing required field: applications";
        return false;
    }

    const JsonValue::Array* applications = RequireArray(applications_it->second, "applications", error);
    if (!applications) {
        return false;
    }

    for (const JsonValue& value : *applications) {
        RegisteredApplication application;
        if (!ParseApplication(value, application, error)) {
            return false;
        }
        out.applications.push_back(std::move(application));
    }

    const auto modules_it = root_object.find("modules");
    if (modules_it == root_object.end()) {
        error = "Missing required field: modules";
        return false;
    }

    const JsonValue::Object* modules_object = RequireObject(modules_it->second, "modules", error);
    if (!modules_object) {
        return false;
    }

    static const std::unordered_set<std::string> allowed_modules = {"depthxr", "pivotxr"};
    if (!CheckAllowedKeys(*modules_object, allowed_modules, error)) {
        return false;
    }

    const auto depth_it = modules_object->find("depthxr");
    if (depth_it == modules_object->end()) {
        error = "Missing required field: modules.depthxr";
        return false;
    }
    const JsonValue::Object* depth_object = RequireObject(depth_it->second, "modules.depthxr", error);
    if (!depth_object || !ParseDepthModule(*depth_object, out.depthxr, error)) {
        return false;
    }

    const auto pivot_it = modules_object->find("pivotxr");
    if (pivot_it == modules_object->end()) {
        error = "Missing required field: modules.pivotxr";
        return false;
    }
    const JsonValue::Object* pivot_object = RequireObject(pivot_it->second, "modules.pivotxr", error);
    if (!pivot_object || !ParsePivotModule(*pivot_object, out.pivotxr, error)) {
        return false;
    }

    return true;
}

} // namespace

ParseResult ParseConfig(std::string_view json_text) {
    ParseResult result;

    JsonParser parser(json_text);
    JsonValue root = parser.Parse();
    if (!parser.ok()) {
        result.error = parser.error();
        return result;
    }

    std::string error;
    const JsonValue::Object* root_object = RequireObject(root, "root", error);
    if (!root_object) {
        result.error = error;
        return result;
    }

    const auto version_it = root_object->find("version");
    if (version_it == root_object->end() || !version_it->second.IsNumber()) {
        result.error = "version must be a number";
        return result;
    }

    const int version = static_cast<int>(version_it->second.AsNumber());
    if (version != 3) {
        result.error = "Unsupported config version. Expected version 3.";
        return result;
    }

    result.document.version = 3;
    if (!ParseVectorDocument(*root_object, result.document, error)) {
        result.error = error;
        return result;
    }

    result.document.depthxr.defaults.enabled = result.document.depthxr.enabled;
    result.ok = true;
    return result;
}

ParseResult LoadConfigFromFile(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) {
        return {.ok = false, .error = "Unable to open config file: " + path.string()};
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return ParseConfig(buffer.str());
}

} // namespace depthxr
