#include "depthxr/config_parser.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
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

    bool IsNull() const { return std::holds_alternative<std::nullptr_t>(storage_); }
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

    bool ok() const {
        return ok_;
    }

    const std::string& error() const {
        return error_;
    }

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
        error = key + " must be one of: off, error, info, debug";
        return false;
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

bool ParseGlobal(const JsonValue::Object& object, ResolvedSettings& out, std::string& error) {
    static const std::unordered_set<std::string> allowed = {
        "enabled",
        "stereoBoostEnabled",
        "worldScaleEnabled",
        "fovScaleEnabled",
        "stereoBoost",
        "worldScale",
        "fovScale",
        "logLevel",
    };

    if (!CheckAllowedKeys(object, allowed, error)) {
        return false;
    }

    if (!ReadRequiredBool(object, "enabled", out.enabled, error) ||
        !ReadRequiredBool(object, "stereoBoostEnabled", out.stereo_boost_enabled, error) ||
        !ReadRequiredBool(object, "worldScaleEnabled", out.world_scale_enabled, error) ||
        !ReadRequiredBool(object, "fovScaleEnabled", out.fov_scale_enabled, error) ||
        !ReadRequiredNumber(object, "stereoBoost", out.stereo_boost, error) ||
        !ReadRequiredNumber(object, "worldScale", out.world_scale, error) ||
        !ReadRequiredNumber(object, "fovScale", out.fov_scale, error)) {
        return false;
    }

    std::optional<LogLevel> level;
    if (!ReadOptionalLogLevel(object, "logLevel", level, error) || !level) {
        if (error.empty()) {
            error = "Missing required field: logLevel";
        }
        return false;
    }
    out.log_level = *level;

    return true;
}

bool ParseSettingsOverride(const JsonValue::Object& object, SettingsOverride& out, std::string& error) {
    static const std::unordered_set<std::string> allowed = {
        "match",
        "enabled",
        "stereoBoostEnabled",
        "worldScaleEnabled",
        "fovScaleEnabled",
        "stereoBoost",
        "worldScale",
        "fovScale",
        "logLevel",
    };

    if (!CheckAllowedKeys(object, allowed, error)) {
        return false;
    }

    return ReadOptionalBool(object, "enabled", out.enabled, error) &&
           ReadOptionalBool(object, "stereoBoostEnabled", out.stereo_boost_enabled, error) &&
           ReadOptionalBool(object, "worldScaleEnabled", out.world_scale_enabled, error) &&
           ReadOptionalBool(object, "fovScaleEnabled", out.fov_scale_enabled, error) &&
           ReadOptionalNumber(object, "stereoBoost", out.stereo_boost, error) &&
           ReadOptionalNumber(object, "worldScale", out.world_scale, error) &&
           ReadOptionalNumber(object, "fovScale", out.fov_scale, error) &&
           ReadOptionalLogLevel(object, "logLevel", out.log_level, error);
}

bool ParseProfile(const JsonValue& value, Profile& out, std::string& error) {
    const JsonValue::Object* object = RequireObject(value, "profile", error);
    if (!object) {
        return false;
    }

    if (!ParseSettingsOverride(*object, out.settings, error)) {
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

    static const std::unordered_set<std::string> allowed_root = {"version", "global", "profiles"};
    if (!CheckAllowedKeys(*root_object, allowed_root, error)) {
        result.error = error;
        return result;
    }

    const auto version_it = root_object->find("version");
    if (version_it == root_object->end() || !version_it->second.IsNumber()) {
        result.error = "version must be a number";
        return result;
    }
    result.document.version = static_cast<int>(version_it->second.AsNumber());

    const auto global_it = root_object->find("global");
    if (global_it == root_object->end()) {
        result.error = "Missing required field: global";
        return result;
    }
    const JsonValue::Object* global_object = RequireObject(global_it->second, "global", error);
    if (!global_object || !ParseGlobal(*global_object, result.document.global, error)) {
        result.error = error;
        return result;
    }

    const auto profiles_it = root_object->find("profiles");
    if (profiles_it == root_object->end()) {
        result.error = "Missing required field: profiles";
        return result;
    }
    const JsonValue::Array* profiles = RequireArray(profiles_it->second, "profiles", error);
    if (!profiles) {
        result.error = error;
        return result;
    }

    for (const JsonValue& value : *profiles) {
        Profile profile;
        if (!ParseProfile(value, profile, error)) {
            result.error = error;
            return result;
        }
        result.document.profiles.push_back(std::move(profile));
    }

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
