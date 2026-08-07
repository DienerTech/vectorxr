#include "depthxr/runtime_relay.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace depthxr {
namespace {

std::optional<std::size_t> ValueStart(std::string_view content, std::string_view key) {
    const std::string token = "\"" + std::string(key) + "\"";
    std::size_t position = content.find(token);
    if (position == std::string_view::npos) return std::nullopt;
    position = content.find(':', position + token.size());
    if (position == std::string_view::npos) return std::nullopt;
    ++position;
    while (position < content.size() && (content[position] == ' ' || content[position] == '\t' || content[position] == '\r' || content[position] == '\n')) ++position;
    return position;
}

std::optional<std::uint64_t> ReadUnsigned(std::string_view content, std::string_view key) {
    const auto start = ValueStart(content, key);
    if (!start || *start >= content.size() || content[*start] < '0' || content[*start] > '9') return std::nullopt;
    std::uint64_t value = 0;
    std::size_t position = *start;
    while (position < content.size() && content[position] >= '0' && content[position] <= '9') {
        const std::uint64_t digit = static_cast<std::uint64_t>(content[position] - '0');
        if (value > (UINT64_MAX - digit) / 10) return std::nullopt;
        value = value * 10 + digit;
        ++position;
    }
    return value;
}

std::optional<bool> ReadBool(std::string_view content, std::string_view key) {
    const auto start = ValueStart(content, key);
    if (!start) return std::nullopt;
    if (content.substr(*start, 4) == "true") return true;
    if (content.substr(*start, 5) == "false") return false;
    return std::nullopt;
}

std::optional<std::string> ReadString(std::string_view content, std::string_view key) {
    const auto start = ValueStart(content, key);
    if (!start || *start >= content.size() || content[*start] != '"') return std::nullopt;
    std::string result;
    for (std::size_t position = *start + 1; position < content.size(); ++position) {
        const char character = content[position];
        if (character == '"') return result;
        if (character == '\\') {
            if (++position >= content.size()) return std::nullopt;
            const char escaped = content[position];
            if (escaped == '"' || escaped == '\\' || escaped == '/') result.push_back(escaped);
            else if (escaped == 'n') result.push_back('\n');
            else if (escaped == 'r') result.push_back('\r');
            else if (escaped == 't') result.push_back('\t');
            else return std::nullopt;
        } else result.push_back(character);
    }
    return std::nullopt;
}

std::string EscapeJson(std::string_view value) {
    std::string escaped;
    for (const char character : value) {
        switch (character) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default: escaped.push_back(character); break;
        }
    }
    return escaped;
}

bool WriteAtomically(const std::filesystem::path& path, const std::string& content, std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) { if (error) *error = "Unable to create runtime relay directory: " + ec.message(); return false; }
    const std::filesystem::path temporary_path = path.string() + ".tmp." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "." + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    {
        std::ofstream stream(temporary_path, std::ios::trunc);
        if (!stream) { if (error) *error = "Unable to write runtime relay temporary file"; return false; }
        stream << content;
    }
#if defined(_WIN32)
    if (!MoveFileExW(temporary_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD move_error = GetLastError();
        std::filesystem::remove(temporary_path, ec);
        if (error) *error = "Unable to replace runtime relay file: Win32 error " + std::to_string(move_error);
        return false;
    }
#else
    std::filesystem::remove(path, ec);
    ec.clear();
    std::filesystem::rename(temporary_path, path, ec);
    if (ec) { std::filesystem::remove(temporary_path, ec); if (error) *error = "Unable to replace runtime relay file: " + ec.message(); return false; }
#endif
    return true;
}

} // namespace

std::filesystem::path ResolveRuntimeRelayRoot() {
    if (const char* env_path = std::getenv("VECTORXR_RUNTIME_RELAY_PATH"); env_path && *env_path != '\0') return std::filesystem::path(env_path);
#if defined(_WIN32)
    if (const char* local_app_data = std::getenv("LOCALAPPDATA"); local_app_data && *local_app_data != '\0') return std::filesystem::path(local_app_data) / "VectorXR" / "runtime";
#endif
    return std::filesystem::current_path() / "runtime";
}

std::filesystem::path RuntimeControlPath(const std::filesystem::path& root, const std::string& session_id) { return root / "control" / (session_id + ".json"); }
std::filesystem::path RuntimeStatusPath(const std::filesystem::path& root, const std::string& session_id) { return root / "status" / (session_id + ".json"); }

std::uint64_t RuntimeRelayUnixMilliseconds() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

bool ReadRuntimeControl(const std::filesystem::path& path, RuntimeControlDocument* document, std::string* error) {
    std::ifstream stream(path);
    if (!stream) return false;
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    const std::string content = buffer.str();
    const auto protocol_version = ReadUnsigned(content, "protocolVersion");
    const auto target_session_id = ReadString(content, "targetSessionId");
    const auto revision = ReadUnsigned(content, "revision");
    const auto expires = ReadUnsigned(content, "expiresAtUnixMilliseconds");
    const auto diagnostic = ReadBool(content, "quadviewsDiagnosticVisualization");
    if (!protocol_version || !target_session_id || !revision || !expires || !diagnostic) { if (error) *error = "Runtime control document is missing or has invalid required fields"; return false; }
    if (*protocol_version != kRuntimeRelayProtocolVersion) { if (error) *error = "Unsupported runtime relay protocol version"; return false; }
    document->protocol_version = static_cast<std::uint32_t>(*protocol_version);
    document->target_session_id = *target_session_id;
    document->revision = *revision;
    document->expires_at_unix_milliseconds = *expires;
    document->quadviews_diagnostic_visualization = *diagnostic;
    return true;
}

bool WriteRuntimeStatus(const std::filesystem::path& path, const RuntimeStatusDocument& document, std::string* error) {
    std::ostringstream stream;
    stream << "{\n"
           << "  \"protocolVersion\": " << kRuntimeRelayProtocolVersion << ",\n"
           << "  \"sessionId\": \"" << EscapeJson(document.session_id) << "\",\n"
           << "  \"processId\": " << document.process_id << ",\n"
           << "  \"application\": \"" << EscapeJson(document.application) << "\",\n"
           << "  \"updatedAtUnixMilliseconds\": " << document.updated_at_unix_milliseconds << ",\n"
           << "  \"acknowledgedRevision\": " << document.acknowledged_revision << ",\n"
           << "  \"capabilities\": { \"quadviewsDiagnosticVisualization\": " << (document.quadviews_diagnostic_visualization_available ? "true" : "false") << " },\n"
           << "  \"state\": { \"quadviewsDiagnosticVisualization\": " << (document.quadviews_diagnostic_visualization_enabled ? "true" : "false") << " }\n"
           << "}\n";
    return WriteAtomically(path, stream.str(), error);
}

} // namespace depthxr

