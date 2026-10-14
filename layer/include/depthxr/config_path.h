#pragma once

#include <filesystem>
#include <string>

namespace depthxr {

std::filesystem::path ResolveConfigPath();
std::filesystem::path ResolveSeenAppsPath();
std::filesystem::path ResolveLogPath();

// %LOCALAPPDATA%, read as UTF-16 like the app reads it: a profile folder
// outside the ANSI code page (a Cyrillic user name on an English-locale
// system) is the same folder for both. Empty when unset.
std::filesystem::path LocalAppDataPath();
// A path from an environment variable (the VECTORXR_*_PATH overrides), read
// as UTF-16. Empty when unset.
std::filesystem::path EnvironmentPath(const wchar_t* name);
// A path as UTF-8, for log and error text. path::string() throws for a
// character outside the ANSI code page; this never throws.
std::string PathToUtf8(const std::filesystem::path& path);

} // namespace depthxr
