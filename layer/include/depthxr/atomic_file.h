#pragma once

#include <cstdint>
#include <filesystem>

namespace depthxr {

// Moves a fully written temporary file over `path` in one step (MoveFileExW
// with REPLACE_EXISTING and WRITE_THROUGH). The old file is never deleted
// first, so a failed replace keeps it. Antivirus and indexing can briefly open
// the destination without delete sharing: access-denied, sharing and lock
// violations are retried, about 60 ms in all. On failure the temporary is
// removed and `error_code` (when given) holds the last Win32 error.
bool ReplaceFileAtomically(const std::filesystem::path& temporary_path,
                           const std::filesystem::path& path,
                           std::uint32_t* error_code = nullptr);

// A temporary name beside `path` no other writer (thread or process) uses.
std::filesystem::path UniqueTemporaryPath(const std::filesystem::path& path);

} // namespace depthxr
