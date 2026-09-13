#pragma once

#include <cstdint>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace depthxr {

struct TurboRecoveryRecord {
    std::string fingerprint;
    std::string application;
    std::string runtime;
    std::string runtime_version;
    std::string system_name;
    std::string graphics_api;
    std::string mode;
    std::string reason;
    std::string state{"armed"};
    std::uint32_t process_id{0};
    std::uint64_t process_created{0};
    std::uint64_t updated_at{0};
};

std::uint64_t TurboProcessCreationTime();
// Unknown/access-denied counts as alive: never quarantine another live process.
bool TurboProcessAlive(std::uint32_t pid, std::uint64_t created);

// The caller serializes this object. No destructor clears the marker: a crash,
// forced exit or skipped OpenXR cleanup must leave evidence for the next run.
class TurboRecoveryGuard {
public:
    using ProcessAlive = std::function<bool(std::uint32_t, std::uint64_t)>;
    void Configure(std::filesystem::path root, TurboRecoveryRecord identity);
    bool Prepare(bool enabled, bool retry, const std::string& mode,
                 const ProcessAlive& alive = TurboProcessAlive);
    void Fail(const std::string& reason);
    void Complete();
    bool Blocked() const { return blocked_; }
    bool ConsumeClearRequest();
    const std::string& Reason() const { return reason_; }
private:
    bool Write();
    std::filesystem::path root_;
    std::filesystem::path own_path_;
    TurboRecoveryRecord record_;
    std::vector<std::filesystem::path> interrupted_;
    bool checked_{false};
    bool armed_{false};
    bool blocked_{false};
    bool failed_{false};
    std::chrono::steady_clock::time_point last_clear_check_{};
    std::string reason_;
};

} // namespace depthxr
