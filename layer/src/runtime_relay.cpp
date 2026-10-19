#include "depthxr/runtime_relay.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string_view>
#include <thread>

#include "depthxr/atomic_file.h"
#include "depthxr/config_path.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace depthxr {
namespace {

// An ended session's files stay this long: Export Debug includes them, the
// last state of a session that crashed. The app removes any relay file
// after three days.
constexpr std::chrono::hours kEndedSessionFileAge{24};

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
    const std::filesystem::path temporary_path = UniqueTemporaryPath(path);
    {
        std::ofstream stream(temporary_path, std::ios::trunc);
        if (!stream) { if (error) *error = "Unable to write runtime relay temporary file"; return false; }
        stream << content;
    }
    std::uint32_t move_error = 0;
    if (!ReplaceFileAtomically(temporary_path, path, &move_error)) {
        if (error) *error = "Unable to replace runtime relay file: Win32 error " + std::to_string(move_error);
        return false;
    }
    return true;
}

// "<pid>-<unix ms>-..." from a status or control file name (or its temporary).
bool ParseSessionFileName(const std::filesystem::path& path, std::uint32_t* process_id, std::uint64_t* started) {
    const std::wstring name = path.filename().wstring();
    std::size_t position = 0;
    const auto read_number = [&](std::uint64_t limit, std::uint64_t* value) {
        const std::size_t start = position;
        *value = 0;
        while (position < name.size() && name[position] >= L'0' && name[position] <= L'9') {
            const std::uint64_t digit = static_cast<std::uint64_t>(name[position] - L'0');
            if (*value > (limit - digit) / 10) return false;
            *value = *value * 10 + digit;
            ++position;
        }
        return position > start && position < name.size() && name[position++] == L'-';
    };
    std::uint64_t pid = 0;
    if (!read_number(UINT32_MAX, &pid) || !read_number(UINT64_MAX, started)) return false;
    *process_id = static_cast<std::uint32_t>(pid);
    return name.find(L".json") != std::wstring::npos;
}

} // namespace

bool RuntimeRelaySessionAlive(std::uint32_t process_id, std::uint64_t started_unix_milliseconds) {
#if defined(_WIN32)
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (!process) return GetLastError() != ERROR_INVALID_PARAMETER;
    DWORD exit_code = 0;
    const bool running = GetExitCodeProcess(process, &exit_code) == FALSE || exit_code == STILL_ACTIVE;
    FILETIME created{}, exited{}, kernel{}, user{};
    bool same = true;
    if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        // FILETIME counts 100 ns since 1601; 11644473600000 ms separate the epochs.
        const std::uint64_t ticks = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        const std::uint64_t created_unix_milliseconds = ticks / 10000 - 11644473600000ull;
        same = created_unix_milliseconds <= started_unix_milliseconds;
    }
    CloseHandle(process);
    return running && same;
#else
    (void)process_id;
    (void)started_unix_milliseconds;
    return true;
#endif
}

std::size_t PruneRuntimeRelayFiles(const std::filesystem::path& root, const RuntimeRelaySessionAliveCheck& alive) {
    std::size_t removed = 0;
    for (const char* folder : {"status", "control"}) {
        std::error_code ec;
        const std::filesystem::path directory = root / folder;
        if (!std::filesystem::is_directory(directory, ec)) continue;
        for (auto iterator = std::filesystem::directory_iterator(directory, ec);
             !ec && iterator != std::filesystem::directory_iterator(); iterator.increment(ec)) {
            std::uint32_t process_id = 0;
            std::uint64_t started = 0;
            if (!ParseSessionFileName(iterator->path(), &process_id, &started)) continue;
            std::error_code time_error;
            const auto written = iterator->last_write_time(time_error);
            if (time_error || std::filesystem::file_time_type::clock::now() - written < kEndedSessionFileAge ||
                alive(process_id, started)) {
                continue;
            }
            std::error_code remove_error;
            if (std::filesystem::remove(iterator->path(), remove_error)) ++removed;
        }
    }
    return removed;
}

std::filesystem::path ResolveRuntimeRelayRoot() {
    if (std::filesystem::path env_path = EnvironmentPath(L"VECTORXR_RUNTIME_RELAY_PATH"); !env_path.empty()) return env_path;
#if defined(_WIN32)
    if (const std::filesystem::path local_app_data = LocalAppDataPath(); !local_app_data.empty()) return local_app_data / "VectorXR" / "runtime";
#endif
    return std::filesystem::current_path() / "runtime";
}

std::filesystem::path RuntimeControlPath(const std::filesystem::path& root, const std::string& session_id) { return root / "control" / (session_id + ".json"); }
std::filesystem::path RuntimeStatusPath(const std::filesystem::path& root, const std::string& session_id) { return root / "status" / (session_id + ".json"); }

std::filesystem::path InputRescanRequestPath(const std::filesystem::path& root) { return root / "input-rescan.json"; }

bool ReadInputRescanRequest(const std::filesystem::path& path, std::uint64_t* requested_at_unix_milliseconds) {
    std::ifstream stream(path);
    if (!stream) return false;
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    const std::string content = buffer.str();
    const auto protocol_version = ReadUnsigned(content, "protocolVersion");
    const auto requested_at = ReadUnsigned(content, "requestedAtUnixMilliseconds");
    if (!protocol_version || *protocol_version != kRuntimeRelayProtocolVersion || !requested_at || *requested_at == 0) {
        return false;
    }
    *requested_at_unix_milliseconds = *requested_at;
    return true;
}

bool InputRescanRequestDue(std::uint64_t requested_at, std::uint64_t last_taken, std::uint64_t now) {
    return requested_at > last_taken && requested_at + kInputRescanFreshMilliseconds >= now;
}

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
    // A game may set a global locale with a decimal comma or digit grouping.
    stream.imbue(std::locale::classic());
    stream << "{\n"
           << "  \"protocolVersion\": " << kRuntimeRelayProtocolVersion << ",\n"
           << "  \"sessionId\": \"" << EscapeJson(document.session_id) << "\",\n"
           << "  \"processId\": " << document.process_id << ",\n"
           << "  \"application\": \"" << EscapeJson(document.application) << "\",\n"
           << "  \"updatedAtUnixMilliseconds\": " << document.updated_at_unix_milliseconds << ",\n"
           << "  \"acknowledgedRevision\": " << document.acknowledged_revision << ",\n"
           << "  \"capabilities\": { \"quadviewsDiagnosticVisualization\": " << (document.quadviews_diagnostic_visualization_available ? "true" : "false") << ", \"osd\": " << (document.osd_available ? "true" : "false") << " },\n"
           << "  \"state\": { \"quadviewsDiagnosticVisualization\": " << (document.quadviews_diagnostic_visualization_enabled ? "true" : "false")
           << ", \"osdVisible\": " << (document.osd_visible ? "true" : "false")
           << ", \"osdCompact\": " << (document.osd_compact ? "true" : "false")
           << ", \"osdMessage\": \"" << EscapeJson(document.osd_message) << "\""
           << ", \"turboState\": \"" << EscapeJson(document.turbo_state)
           << "\", \"turboReason\": \"" << EscapeJson(document.turbo_reason)
           << "\", \"framegenState\": \"" << EscapeJson(document.framegen_state)
           << "\", \"framegenReason\": \"" << EscapeJson(document.framegen_reason)
           << "\", \"framegenDepth\": \"" << EscapeJson(document.framegen_depth) << "\" },\n";
    if (const auto& rates = document.framegen_rates) {
        stream << std::fixed << std::setprecision(1) << "  \"framegen\": { \"mode\": \"" << EscapeJson(rates->mode)
               << "\", \"frameRate\": \"" << EscapeJson(rates->frame_rate)
               << "\", \"generating\": " << (rates->generating ? "true" : "false")
               << ", \"multiplier\": " << rates->multiplier << ", \"refreshHz\": " << rates->refresh_hz
               << ", \"holdFps\": " << rates->hold_fps << ", \"appFps\": " << rates->app_fps
               << ", \"outputFps\": " << rates->output_fps << ", \"distinctFps\": " << rates->distinct_fps
               << " },\n";
    }
    stream << "  \"quadviewsDimensionsAt\": " << document.quadviews_dimensions_at << ",\n"
           << "  \"quadviewsDimensions\": [";
    for (std::size_t i = 0; i < document.quadviews_dimensions.size(); ++i) {
        const auto& view = document.quadviews_dimensions[i];
        if (i) stream << ",";
        stream << "{\"width\":" << view.width << ",\"height\":" << view.height
               << ",\"allocatedWidth\":" << view.allocated_width << ",\"allocatedHeight\":" << view.allocated_height << "}";
    }
    stream << "]\n}\n";
    return WriteAtomically(path, stream.str(), error);
}

} // namespace depthxr

