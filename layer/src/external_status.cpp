#include "depthxr/external_status.h"

#include <algorithm>
#include <chrono>
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

// Real frames taking at most this share more refreshes than aimed for still
// hold the target (a slot missed now and then is not a slower game).
constexpr double kHoldingTolerance = 1.1;
// Status documents are well under this; anything longer is not one.
constexpr std::size_t kMaxDocumentBytes = 64 * 1024;

std::string EscapeJson(std::string_view value) {
    std::string escaped;
    for (const char character : value) {
        switch (character) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (static_cast<unsigned char>(character) < 0x20) {
                static constexpr char kHex[] = "0123456789abcdef";
                escaped += "\\u00";
                escaped.push_back(kHex[(character >> 4) & 0xF]);
                escaped.push_back(kHex[character & 0xF]);
            } else {
                escaped.push_back(character);
            }
            break;
        }
    }
    return escaped;
}

// The number after "processId": in a document; none for anything else.
std::optional<std::uint64_t> ReadProcessId(std::string_view content) {
    constexpr std::string_view kToken = "\"processId\"";
    std::size_t position = content.find(kToken);
    if (position == std::string_view::npos) return std::nullopt;
    position = content.find(':', position + kToken.size());
    if (position == std::string_view::npos) return std::nullopt;
    ++position;
    while (position < content.size() && (content[position] == ' ' || content[position] == '\t' ||
                                         content[position] == '\r' || content[position] == '\n')) {
        ++position;
    }
    if (position >= content.size() || content[position] < '0' || content[position] > '9') return std::nullopt;
    std::uint64_t value = 0;
    while (position < content.size() && content[position] >= '0' && content[position] <= '9') {
        const std::uint64_t digit = static_cast<std::uint64_t>(content[position] - '0');
        if (value > (UINT64_MAX - digit) / 10) return std::nullopt;
        value = value * 10 + digit;
        ++position;
    }
    return value;
}

} // namespace

void ExternalFrameGenTarget(ExternalFrameGenStatus* framegen, double grid_hz) {
    const std::uint32_t slots = framegen->generating ? std::max<std::uint32_t>(framegen->multiplier, 1) : 1;
    const double grid = grid_hz > 0.0 ? grid_hz : framegen->refresh_hz;
    framegen->target_app_fps = grid > 0.0 ? grid / slots : 0.0;
    // refreshes_per_real_frame is in refreshes; the multiplier counts grid periods.
    const double periods = framegen->refresh_hz > 0.0 && grid > 0.0
                               ? framegen->refreshes_per_real_frame * grid / framegen->refresh_hz
                               : framegen->refreshes_per_real_frame;
    framegen->holding = framegen->target_app_fps > 0.0 && periods > 0.0 && periods <= kHoldingTolerance * slots;
}

std::filesystem::path ResolveExternalStatusPath() {
    if (std::filesystem::path env_path = EnvironmentPath(L"VECTORXR_EXTERNAL_STATUS_PATH"); !env_path.empty()) {
        return env_path;
    }
#if defined(_WIN32)
    if (const std::filesystem::path local_app_data = LocalAppDataPath(); !local_app_data.empty()) {
        return local_app_data / "VectorXR" / "api" / "status.json";
    }
#endif
    return std::filesystem::current_path() / "api" / "status.json";
}

std::string FormatExternalStatus(const ExternalStatusDocument& document) {
    std::ostringstream stream;
    // A game may set a global locale with a decimal comma or digit grouping.
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(2);
    stream << "{\n"
           << "  \"schema\": \"" << kExternalStatusSchema << "\",\n"
           << "  \"version\": " << kExternalStatusVersion << ",\n"
           << "  \"processId\": " << document.process_id << ",\n"
           << "  \"application\": \"" << EscapeJson(document.application) << "\",\n"
           << "  \"sessionId\": \"" << EscapeJson(document.session_id) << "\",\n"
           << "  \"updatedAtUnixMilliseconds\": " << document.updated_at_unix_milliseconds;
    if (const auto& framegen = document.framegen) {
        stream << ",\n  \"framegen\": {\n"
               << "    \"state\": \"" << EscapeJson(framegen->state) << "\",\n"
               << "    \"mode\": \"" << EscapeJson(framegen->mode) << "\",\n"
               << "    \"frameRate\": \"" << EscapeJson(framegen->frame_rate) << "\",\n"
               << "    \"generating\": " << (framegen->generating ? "true" : "false") << ",\n"
               << "    \"multiplier\": " << framegen->multiplier << ",\n"
               << "    \"refreshHz\": " << framegen->refresh_hz << ",\n"
               << "    \"targetAppFps\": " << framegen->target_app_fps << ",\n"
               << "    \"appFps\": " << framegen->app_fps << ",\n"
               << "    \"outputFps\": " << framegen->output_fps << ",\n"
               << "    \"distinctFps\": " << framegen->distinct_fps << ",\n"
               << "    \"refreshesPerRealFrame\": " << framegen->refreshes_per_real_frame << ",\n"
               << "    \"holding\": " << (framegen->holding ? "true" : "false");
        if (const auto& automatic = framegen->automatic) {
            stream << ",\n    \"autoStep\": " << automatic->step << ",\n"
                   << "    \"autoStepUpBelowFps\": " << automatic->step_up_below_fps << ",\n"
                   << "    \"autoStepDownAboveFps\": " << automatic->step_down_above_fps << ",\n"
                   << "    \"autoGameOwnFps\": " << automatic->game_own_fps;
        }
        stream << "\n  }";
    }
    stream << "\n}\n";
    return stream.str();
}

bool WriteExternalStatus(const std::filesystem::path& path, const ExternalStatusDocument& document,
                         std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        if (error) *error = "Unable to create the status API directory: " + ec.message();
        return false;
    }
    const std::filesystem::path temporary_path = UniqueTemporaryPath(path);
    {
        std::ofstream stream(temporary_path, std::ios::binary | std::ios::trunc);
        if (!stream) {
            if (error) *error = "Unable to write the status API temporary file";
            return false;
        }
        stream << FormatExternalStatus(document);
        stream.close();
        if (!stream) {
            std::filesystem::remove(temporary_path, ec);
            if (error) *error = "Unable to write the status API temporary file";
            return false;
        }
    }
    std::uint32_t move_error = 0;
    if (!ReplaceFileAtomically(temporary_path, path, &move_error)) {
        if (error) *error = "Unable to replace the status API file: Win32 error " + std::to_string(move_error);
        return false;
    }
    return true;
}

ExternalStatusRemoval RemoveExternalStatusIfOwned(const std::filesystem::path& path, std::uint32_t process_id) {
#if defined(_WIN32)
    // Opened without delete sharing, another game's replace cannot take the
    // file between the check and the removal (ReplaceFileAtomically retries
    // until it is closed). A reader holding it without delete sharing makes
    // the open fail for a moment: retried as the replace is.
    HANDLE file = INVALID_HANDLE_VALUE;
    constexpr int kOpenAttempts = 6;
    for (int attempt = 0; attempt < kOpenAttempts; ++attempt) {
        file = CreateFileW(path.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        const DWORD open_error = GetLastError();
        const bool transient = open_error == ERROR_ACCESS_DENIED || open_error == ERROR_SHARING_VIOLATION ||
                               open_error == ERROR_LOCK_VIOLATION;
        if (!transient) return ExternalStatusRemoval::kNotOwned;
        if (attempt + 1 == kOpenAttempts) return ExternalStatusRemoval::kBusy;
        std::this_thread::sleep_for(std::chrono::milliseconds(2u << attempt));
    }
    char buffer[kMaxDocumentBytes];
    DWORD size = 0;
    DWORD read = 0;
    while (size < sizeof(buffer) && ReadFile(file, buffer + size, sizeof(buffer) - size, &read, nullptr) && read > 0) {
        size += read;
    }
    ExternalStatusRemoval result = ExternalStatusRemoval::kNotOwned;
    if (const auto owner = ReadProcessId(std::string_view(buffer, size)); owner && *owner == process_id) {
        // POSIX semantics take the name away at once, even from a reader
        // that has it open with delete sharing (the next game's replace
        // would fail while a classic deletion is pending); older Windows
        // has only the classic one.
        FILE_DISPOSITION_INFO_EX posix{};
        posix.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS;
        FILE_DISPOSITION_INFO classic{};
        classic.DeleteFile = TRUE;
        const bool removed =
            SetFileInformationByHandle(file, FileDispositionInfoEx, &posix, sizeof(posix)) != FALSE ||
            SetFileInformationByHandle(file, FileDispositionInfo, &classic, sizeof(classic)) != FALSE;
        result = removed ? ExternalStatusRemoval::kRemoved : ExternalStatusRemoval::kBusy;
    }
    CloseHandle(file);
    return result;
#else
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return ExternalStatusRemoval::kNotOwned;
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    stream.close();
    const auto owner = ReadProcessId(buffer.str());
    if (!owner || *owner != process_id) return ExternalStatusRemoval::kNotOwned;
    std::error_code ec;
    return std::filesystem::remove(path, ec) ? ExternalStatusRemoval::kRemoved : ExternalStatusRemoval::kBusy;
#endif
}

} // namespace depthxr
