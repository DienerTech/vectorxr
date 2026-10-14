#include "depthxr/atomic_file.h"

#include <chrono>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace depthxr {

bool ReplaceFileAtomically(const std::filesystem::path& temporary_path,
                           const std::filesystem::path& path,
                           std::uint32_t* error_code) {
    std::error_code ec;
#if defined(_WIN32)
    DWORD move_error = ERROR_SUCCESS;
    constexpr int kMoveAttempts = 6;
    for (int attempt = 0; attempt < kMoveAttempts; ++attempt) {
        if (MoveFileExW(temporary_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return true;
        }
        move_error = GetLastError();
        const bool transient = move_error == ERROR_ACCESS_DENIED || move_error == ERROR_SHARING_VIOLATION ||
                               move_error == ERROR_LOCK_VIOLATION;
        if (!transient || attempt + 1 == kMoveAttempts) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2u << attempt));
    }
    std::filesystem::remove(temporary_path, ec);
    if (error_code) {
        *error_code = move_error;
    }
    return false;
#else
    // POSIX rename replaces the destination atomically.
    std::filesystem::rename(temporary_path, path, ec);
    if (!ec) {
        return true;
    }
    if (error_code) {
        *error_code = static_cast<std::uint32_t>(ec.value());
    }
    std::filesystem::remove(temporary_path, ec);
    return false;
#endif
}

std::filesystem::path UniqueTemporaryPath(const std::filesystem::path& path) {
    std::filesystem::path temporary_path = path;
    temporary_path += ".tmp." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "." +
                      std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    return temporary_path;
}

} // namespace depthxr
