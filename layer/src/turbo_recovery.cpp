#include "depthxr/turbo_recovery.h"
#include "depthxr/runtime_relay.h"
#include <chrono>
#include <fstream>
#include <sstream>
#include <string_view>
#include <thread>
#include <atomic>
#include <algorithm>
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
        stream.flush();
        if (!stream) { if (error) *error = "Unable to flush recovery record"; return false; }
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


bool ArchiveFault(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) return false;
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    const auto content = buffer.str();
    const auto timestamp = ReadUnsigned(content, "updatedAtUnixMilliseconds");
    if (!timestamp) return false;
    const auto destination = path.parent_path() / "faults" /
        (path.stem().string() + "-" + std::to_string(*timestamp) + ".json");
    std::error_code ec;
    auto cleared = path.parent_path() / "faults-cleared" / destination.filename();
    cleared.replace_extension("txt");
    if (std::filesystem::exists(cleared, ec)) return true;
    if (std::filesystem::exists(destination, ec)) return true;
    return WriteAtomically(destination, content, nullptr);
}

std::uint64_t FileTimeValue(
#if defined(_WIN32)
    FILETIME value
#else
    std::uint64_t value
#endif
) {
#if defined(_WIN32)
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
#else
    return value;
#endif
}
} // namespace

std::uint64_t TurboProcessCreationTime() {
#if defined(_WIN32)
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return FileTimeValue(created);
#endif
    return 0;
}

bool TurboProcessAlive(std::uint32_t pid, std::uint64_t created) {
#if defined(_WIN32)
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!process) return GetLastError() != ERROR_INVALID_PARAMETER;
    const DWORD wait = WaitForSingleObject(process, 0);
    FILETIME start{}, exited{}, kernel{}, user{};
    const bool known = GetProcessTimes(process, &start, &exited, &kernel, &user) != FALSE;
    const bool same = !known || created == 0 || FileTimeValue(start) == created;
    CloseHandle(process);
    return wait != WAIT_OBJECT_0 && same;
#else
    return true;
#endif
}

void TurboRecoveryGuard::Configure(std::filesystem::path root, TurboRecoveryRecord identity) {
    root_ = std::move(root);
    record_ = std::move(identity);
    static std::atomic<std::uint64_t> sequence{0};
    own_path_ = root_ / (std::to_string(record_.process_id) + "-" +
        std::to_string(record_.process_created) + "-" + std::to_string(++sequence) + ".json");
    checked_ = armed_ = blocked_ = failed_ = false;
    interrupted_.clear();
    reason_.clear();
}

bool TurboRecoveryGuard::Write() {
    record_.updated_at = (std::max)(RuntimeRelayUnixMilliseconds(), record_.updated_at + 1);
    std::ostringstream json;
    json << "{\n  \"version\": 1,\n";
    const auto field = [&](const char* key, const std::string& value) {
        json << "  \"" << key << "\": \"" << EscapeJson(value) << "\",\n";
    };
    field("fingerprint", record_.fingerprint);
    field("application", record_.application);
    field("runtime", record_.runtime);
    field("runtimeVersion", record_.runtime_version);
    field("systemName", record_.system_name);
    field("graphicsApi", record_.graphics_api);
    field("mode", record_.mode);
    field("reason", record_.reason);
    field("state", record_.state);
    // Process creation time is a string so JavaScript cannot round the identity.
    field("processCreated", std::to_string(record_.process_created));
    json << "  \"processId\": " << record_.process_id << ",\n"
         << "  \"updatedAtUnixMilliseconds\": " << record_.updated_at << "\n}\n";
    std::string error;
    if (WriteAtomically(own_path_, json.str(), &error)) return true;
    reason_ = "Recovery record could not be saved. Turbo remains off while automatic recovery is enabled.";
    return false;
}

bool TurboRecoveryGuard::Prepare(bool enabled, bool retry, const std::string& mode, const ProcessAlive& alive) {
    if (!enabled) {
        record_.mode = mode;
        std::error_code ec;
        if (armed_) std::filesystem::remove(own_path_, ec);
        armed_ = blocked_ = failed_ = checked_ = false;
        reason_.clear();
        return true;
    }
    if (root_.empty()) return true; // No real session (e.g. frame-loop harness).
    if (retry) {
        if (failed_ && !ArchiveFault(own_path_)) {
            reason_ = "Fault history could not be saved. Clear the block from Turbo Safety after storage is available.";
            return false;
        }
        checked_ = blocked_ = failed_ = false;
        reason_.clear();
    }
    if (!checked_) {
        interrupted_.clear();
        std::error_code ec;
        if (std::filesystem::exists(root_, ec)) {
            for (auto iterator = std::filesystem::directory_iterator(root_, ec);
                 !ec && iterator != std::filesystem::directory_iterator(); iterator.increment(ec)) {
                const auto& entry = *iterator;
                if (entry.path() == own_path_ || entry.path().extension() != ".json") continue;
                if (entry.file_size(ec) > 32 * 1024 || ec) { ec.clear(); continue; }
                std::ifstream stream(entry.path());
                std::ostringstream buffer;
                buffer << stream.rdbuf();
                const auto text = buffer.str();
                if (ReadUnsigned(text, "version") != 1 || ReadString(text, "fingerprint") != record_.fingerprint) continue;
                const auto state = ReadString(text, "state");
                const auto pid = ReadUnsigned(text, "processId");
                const auto created_text = ReadString(text, "processCreated");
                if (!pid || *pid > UINT32_MAX || !created_text || !state) continue;
                std::uint64_t created = 0;
                try { created = std::stoull(*created_text); } catch (...) { continue; }
                if (*state == "failed" || (*state == "armed" && !alive(static_cast<std::uint32_t>(*pid), created))) {
                    ArchiveFault(entry.path());
                    interrupted_.push_back(entry.path());
                }
            }
        }
        if (ec) {
            checked_ = true; // Retry only on user intent, never rescan a broken path every frame.
            blocked_ = true;
            reason_ = "Recovery history could not be read. Turbo remains off while automatic recovery is enabled.";
            return false;
        }
        checked_ = true;
        blocked_ = !retry && !interrupted_.empty();
        if (blocked_) reason_ = "The previous Turbo session ended unexpectedly or encountered runtime errors. Turbo is disabled for this application and runtime. Use the Turbo toggle to retry.";
    }
    if (blocked_) return false;
    if (!armed_ || retry) {
        if (failed_) ArchiveFault(own_path_);
        record_.mode = mode;
        record_.state = "armed";
        record_.reason.clear();
        if (!Write()) { blocked_ = true; return false; }
        armed_ = true;
        // Establish the new marker before clearing the evidence it supersedes.
        for (const auto& path : interrupted_) {
            // Preserve evidence before an explicit retry clears a block.
            if (ArchiveFault(path)) { std::error_code ec; std::filesystem::remove(path, ec); }
        }
        interrupted_.clear();
    }
    return true;
}

void TurboRecoveryGuard::Fail(const std::string& reason) {
    reason_ = reason;
    if (root_.empty()) return;
    if (!armed_) {
        // Opt-outs still retain diagnostics without creating a persistent block.
        record_.state = "observed";
        record_.reason = reason;
        if (Write() && ArchiveFault(own_path_)) {
            std::error_code ec;
            std::filesystem::remove(own_path_, ec);
        }
        return;
    }
    failed_ = true;
    record_.state = "failed";
    record_.reason = reason;
    Write();
    ArchiveFault(own_path_);
}

bool TurboRecoveryGuard::ConsumeClearRequest() {
    if ((!blocked_ && !failed_) || root_.empty()) return false;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_clear_check_ < std::chrono::seconds(1)) return false;
    last_clear_check_ = now;
    std::error_code ec;
    // The UI archives then removes failed markers. Never treat an unreadable
    // directory or a live marker as permission to retry.
    if (failed_) {
        if (std::filesystem::exists(own_path_, ec) || ec) return false;
    } else {
        if (interrupted_.empty()) return false;
        for (const auto& path : interrupted_) {
            if (std::filesystem::exists(path, ec) || ec) return false;
        }
    }
    checked_ = blocked_ = failed_ = armed_ = false;
    reason_.clear();
    return true;
}

void TurboRecoveryGuard::Complete() {
    if (armed_ && !failed_) { std::error_code ec; std::filesystem::remove(own_path_, ec); }
    armed_ = false;
    checked_ = false;
}
} // namespace depthxr
