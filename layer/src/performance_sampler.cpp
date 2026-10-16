#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_4.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wrl/client.h>

#include "depthxr/logger.h"
#include "depthxr/performance_monitor.h"
#include "depthxr/process_exit.h"

#include <cstdio>
#include <exception>

// PerformanceMonitor's sampler thread (performance_monitor.h). pdh.dll and dxgi.dll are loaded from System32 on that
// thread when the row is first shown, so a layer whose row is never shown loads neither, and its imports do not
// change. Neither module is ever unloaded: pdh.dll may keep threads of its own.

namespace depthxr {
namespace {
using Microsoft::WRL::ComPtr;

std::string Hex(std::uint32_t value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%08X", value);
    return buffer;
}

HMODULE SystemModule(const wchar_t* name) {
    return LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
}

// The 3D engines' utilization from the documented "GPU Engine" counter set: one instance per process and engine.
class GpuEngineCounters {
  public:
    ~GpuEngineCounters() { Close(); }

    bool Open(std::string& error) {
        Close();
        static const HMODULE module = SystemModule(L"pdh.dll");
        if (!module) {
            error = "pdh.dll did not load (" + std::to_string(GetLastError()) + ")";
            return false;
        }
        open_ = reinterpret_cast<decltype(&PdhOpenQueryW)>(GetProcAddress(module, "PdhOpenQueryW"));
        add_ = reinterpret_cast<decltype(&PdhAddEnglishCounterW)>(GetProcAddress(module, "PdhAddEnglishCounterW"));
        collect_ = reinterpret_cast<decltype(&PdhCollectQueryData)>(GetProcAddress(module, "PdhCollectQueryData"));
        array_ = reinterpret_cast<decltype(&PdhGetFormattedCounterArrayW)>(
            GetProcAddress(module, "PdhGetFormattedCounterArrayW"));
        close_ = reinterpret_cast<decltype(&PdhCloseQuery)>(GetProcAddress(module, "PdhCloseQuery"));
        if (!open_ || !add_ || !collect_ || !array_ || !close_) {
            error = "pdh.dll lacks the query functions";
            return false;
        }
        PDH_STATUS status = open_(nullptr, 0, &query_);
        if (status != ERROR_SUCCESS) {
            query_ = nullptr;
            error = "PdhOpenQuery failed (" + Hex(status) + ")";
            return false;
        }
        // Only the 3D engines' instances (Task Manager's "3D"); every engine's where the counter set takes no
        // partial wildcard.
        status = add_(query_, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &counter_);
        if (status != ERROR_SUCCESS) {
            status = add_(query_, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter_);
        }
        if (status != ERROR_SUCCESS) {
            error = "the GPU Engine counters are unavailable (" + Hex(status) + ")";
            Close();
            return false;
        }
        // Utilization is a rate: the first collection is its baseline.
        status = collect_(query_);
        if (status != ERROR_SUCCESS && status != PDH_NO_DATA) {
            error = "PdhCollectQueryData failed (" + Hex(status) + ")";
            Close();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool IsOpen() const { return query_ != nullptr; }

    // The instances since the last collection; an empty list when there are none (no 3D work anywhere). Nullopt
    // with `error` when the query failed. Instances that came or went since (churn) have no valid value and are
    // left out.
    std::optional<std::vector<std::pair<GpuEngineInstance, double>>> Read(std::string& error) {
        std::vector<std::pair<GpuEngineInstance, double>> samples;
        PDH_STATUS status = collect_(query_);
        if (status == PDH_NO_DATA) {
            return samples;
        }
        if (status != ERROR_SUCCESS) {
            error = "PdhCollectQueryData failed (" + Hex(status) + ")";
            return std::nullopt;
        }
        constexpr DWORD format = PDH_FMT_DOUBLE | PDH_FMT_NOCAP100;
        DWORD bytes = 0;
        DWORD count = 0;
        status = array_(counter_, format, &bytes, &count, nullptr);
        if (status == PDH_MORE_DATA) {
            // 8-byte units: the items hold doubles; their names follow them in the same buffer.
            buffer_.resize((bytes + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t));
            status = array_(counter_, format, &bytes, &count,
                            reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer_.data()));
        }
        if (status == PDH_NO_DATA || status == PDH_CSTATUS_NO_INSTANCE || (status == ERROR_SUCCESS && !count)) {
            return samples;
        }
        if (status != ERROR_SUCCESS) {
            error = "PdhGetFormattedCounterArray failed (" + Hex(status) + ")";
            return std::nullopt;
        }
        const auto* items = reinterpret_cast<const PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer_.data());
        samples.reserve(count);
        for (DWORD i = 0; i < count; ++i) {
            const PDH_FMT_COUNTERVALUE& value = items[i].FmtValue;
            if ((value.CStatus != PDH_CSTATUS_VALID_DATA && value.CStatus != PDH_CSTATUS_NEW_DATA) ||
                !items[i].szName) {
                continue;
            }
            if (auto instance = ParseGpuEngineInstance(items[i].szName)) {
                samples.emplace_back(std::move(*instance), value.doubleValue);
            }
        }
        return samples;
    }

    void Close() {
        if (query_ && close_) {
            close_(query_);
        }
        query_ = nullptr;
        counter_ = nullptr;
    }

  private:
    decltype(&PdhOpenQueryW) open_{};
    decltype(&PdhAddEnglishCounterW) add_{};
    decltype(&PdhCollectQueryData) collect_{};
    decltype(&PdhGetFormattedCounterArrayW) array_{};
    decltype(&PdhCloseQuery) close_{};
    PDH_HQUERY query_{};
    PDH_HCOUNTER counter_{};
    std::vector<std::uint64_t> buffer_;
};

// The process's local video memory on the session's adapter.
class AdapterMemory {
  public:
    bool Open(std::uint64_t luid, std::string& error) {
        static const HMODULE module = SystemModule(L"dxgi.dll");
        const auto create = module ? reinterpret_cast<decltype(&CreateDXGIFactory1)>(
                                         GetProcAddress(module, "CreateDXGIFactory1"))
                                   : nullptr;
        ComPtr<IDXGIFactory4> factory;
        if (!create || FAILED(create(IID_PPV_ARGS(&factory)))) {
            error = "no DXGI 1.4 factory";
            return false;
        }
        LUID adapter_luid{};
        adapter_luid.LowPart = static_cast<DWORD>(luid & 0xffffffffull);
        adapter_luid.HighPart = static_cast<LONG>(luid >> 32);
        const HRESULT result = factory->EnumAdapterByLuid(adapter_luid, IID_PPV_ARGS(&adapter_));
        if (FAILED(result)) {
            error = "DXGI has no adapter with the session's LUID (" + Hex(static_cast<std::uint32_t>(result)) + ")";
            return false;
        }
        return true;
    }

    std::optional<std::pair<std::uint64_t, std::uint64_t>> Read() const {
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (!adapter_ || FAILED(adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
            return std::nullopt;
        }
        return std::make_pair(info.CurrentUsage, info.Budget);
    }

  private:
    ComPtr<IDXGIAdapter3> adapter_;
};

std::uint64_t FileTimeTicks(const FILETIME& time) {
    return static_cast<std::uint64_t>(time.dwHighDateTime) << 32 | time.dwLowDateTime;
}

// A thread's CPU time (kernel and user), in 100 ns units; nullopt when it cannot be read.
std::optional<std::uint64_t> ThreadCpuTicks(HANDLE thread) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
        return std::nullopt;
    }
    return FileTimeTicks(kernel) + FileTimeTicks(user);
}

// How busy one of the game's threads is, of one core, between reads. GetThreadTimes counts in clock ticks
// (15.6 ms by default): a half-second read is good to about 3%, which the reading's smoothing evens out.
class ThreadBusy {
  public:
    ~ThreadBusy() { Close(); }

    std::optional<double> Read(std::uint32_t thread, PerformanceClock::time_point now) {
        if (!thread) {
            return std::nullopt;
        }
        if (thread != id_) {
            Close();
            id_ = thread;
            handle_ = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, thread);
        }
        if (!handle_) {
            return std::nullopt;
        }
        const std::optional<std::uint64_t> ticks = ThreadCpuTicks(handle_);
        if (!ticks) {
            return std::nullopt;
        }
        std::optional<double> busy;
        if (last_) {
            const double wall = std::chrono::duration<double>(now - last_->second).count();
            if (wall > 0.0 && *ticks >= last_->first) {
                busy = static_cast<double>(*ticks - last_->first) * 1e-7 / wall * 100.0;
            }
        }
        last_ = std::make_pair(*ticks, now);
        return busy;
    }

  private:
    void Close() {
        if (handle_) {
            CloseHandle(handle_);
        }
        handle_ = nullptr;
        id_ = 0;
        last_.reset();
    }

    std::uint32_t id_{0};
    HANDLE handle_{nullptr};
    std::optional<std::pair<std::uint64_t, PerformanceClock::time_point>> last_;
};

} // namespace

PerformanceMonitor::~PerformanceMonitor() {
    // At a process exit the sampler thread is already gone (process_exit.h): there is nothing to stop, and joining
    // it would wait on a lock it may have held.
    if (ProcessExiting()) {
        if (thread_.joinable()) {
            thread_.detach();
        }
        return;
    }
    EndSession();
}

void PerformanceMonitor::BeginSession(std::optional<std::uint64_t> adapter_luid, FrameGenSource framegen,
                                      Logger* logger) {
    EndSession();
    std::scoped_lock lock(mutex_);
    adapter_luid_ = adapter_luid;
    framegen_ = std::move(framegen);
    logger_ = logger;
    session_ = true;
    start_failed_ = false;
    reading_ = {};
}

void PerformanceMonitor::EndSession() {
    std::thread thread;
    {
        std::scoped_lock lock(mutex_);
        stop_ = true;
        session_ = false;
        wanted_.store(false, std::memory_order_relaxed);
        thread = std::move(thread_);
    }
    cv_.notify_all();
    if (thread.joinable()) {
        thread.join();
    }
    std::scoped_lock lock(mutex_);
    stop_ = false;
    framegen_ = nullptr;
    reading_ = {};
    frames_.Reset();
}

void PerformanceMonitor::SetWanted(bool wanted) {
    std::scoped_lock lock(mutex_);
    wanted_.store(wanted, std::memory_order_relaxed);
    if (wanted && session_ && !thread_.joinable() && !start_failed_) {
        try {
            thread_ = std::thread([this] { Run(); });
        } catch (...) {
            // No thread: the row says what it cannot measure.
            start_failed_ = true;
            if (logger_) {
                logger_->Error("Performance: the sampler thread could not start; the OSD's PERFORMANCE row shows "
                               "only what the game's frames tell.");
            }
        }
    }
    cv_.notify_all();
}

PerformanceReading PerformanceMonitor::Reading() const {
    std::scoped_lock lock(mutex_);
    return reading_;
}

void PerformanceMonitor::Run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    SetThreadDescription(GetCurrentThread(), L"VectorXR performance sampler");
    std::optional<std::uint64_t> luid;
    FrameGenSource framegen;
    Logger* logger = nullptr;
    {
        std::scoped_lock lock(mutex_);
        luid = adapter_luid_;
        framegen = framegen_;
        logger = logger_;
    }
    const auto log = [logger](const std::string& line) {
        if (logger) {
            logger->Info(line);
        }
    };
    const DWORD pid = GetCurrentProcessId();
    GpuEngineCounters gpu;
    AdapterMemory memory;
    ThreadBusy thread_busy;
    PerformanceModel model;
    PerformanceSummary summary;
    // GPU counters that failed are tried again this long after, quietly: each failure kind is logged once.
    constexpr std::chrono::seconds kGpuRetry{30};
    PerformanceClock::time_point gpu_retry{};
    bool gpu_failed = false;
    bool gpu_open_logged = false;
    bool gpu_read_logged = false;
    std::string gpu_state = "not read yet";
    bool memory_tried = false;
    bool memory_open = false;
    if (!luid) {
        log("Performance: the session's adapter is not known (no Direct3D 11, Direct3D 12 or Vulkan adapter LUID); "
            "video memory is read from Frame Generation where it runs.");
    }
    const auto own_cpu = [] { return ThreadCpuTicks(GetCurrentThread()).value_or(0); };
    PerformanceClock::time_point next = PerformanceClock::now();
    PerformanceClock::time_point summary_start = next;
    std::uint64_t summary_cpu = own_cpu();
    const auto flush = [&](PerformanceClock::time_point now) {
        if (!summary.Samples()) {
            return;
        }
        const double seconds = std::chrono::duration<double>(now - summary_start).count();
        const std::uint64_t cpu = own_cpu();
        const std::optional<double> cpu_percent =
            seconds > 0.0 && cpu >= summary_cpu
                ? std::optional<double>(static_cast<double>(cpu - summary_cpu) * 1e-7 / seconds * 100.0)
                : std::nullopt;
        log(summary.Take(seconds, cpu_percent, gpu_state));
        summary_start = now;
        summary_cpu = cpu;
    };
    for (;;) {
        {
            std::unique_lock lock(mutex_);
            cv_.wait_until(lock, next, [this] { return stop_; });
            if (stop_) {
                break;
            }
            if (!wanted_.load(std::memory_order_relaxed)) {
                // The row went: its window is logged, the counters close, and nothing is sampled until it is back.
                lock.unlock();
                flush(PerformanceClock::now());
                gpu.Close();
                model.Reset();
                lock.lock();
                reading_ = {};
                cv_.wait(lock, [this] { return stop_ || wanted_.load(std::memory_order_relaxed); });
                if (stop_) {
                    break;
                }
                lock.unlock();
                // Frames fed while it was away belong to no window.
                frames_.Reset();
                next = PerformanceClock::now();
                summary_start = next;
                summary_cpu = own_cpu();
                continue;
            }
        }
        const PerformanceClock::time_point start = PerformanceClock::now();
        try {
            PerformanceTick tick;
            tick.now = start;
            tick.frames = frames_.Take();
            if (!gpu.IsOpen() && start >= gpu_retry) {
                std::string error;
                if (gpu.Open(error)) {
                    gpu_state = "opened";
                    gpu_failed = false;
                } else {
                    gpu_failed = true;
                    gpu_state = "unavailable (" + error + ")";
                    gpu_retry = start + kGpuRetry;
                    if (!gpu_open_logged) {
                        gpu_open_logged = true;
                        log("Performance: GPU utilization is unavailable: " + error + "; tried again every " +
                            std::to_string(kGpuRetry.count()) + " s without logging.");
                    }
                }
            } else if (gpu.IsOpen()) {
                std::string error;
                if (const auto samples = gpu.Read(error)) {
                    const GpuEngineUtilization usage = AggregateGpuEngines(*samples, pid, luid);
                    tick.gpu_percent = usage.total;
                    tick.gpu_game_percent = usage.game;
                    gpu_state = std::to_string(usage.instances) + " 3D instances of " +
                                std::to_string(samples->size());
                } else {
                    gpu_failed = true;
                    gpu_state = "failed (" + error + ")";
                    gpu.Close();
                    gpu_retry = start + kGpuRetry;
                    if (!gpu_read_logged) {
                        gpu_read_logged = true;
                        log("Performance: reading GPU utilization failed: " + error + "; tried again every " +
                            std::to_string(kGpuRetry.count()) + " s without logging.");
                    }
                }
            }
            tick.gpu_unavailable = gpu_failed;
            if (luid && !memory_tried) {
                memory_tried = true;
                std::string error;
                memory_open = memory.Open(*luid, error);
                if (!memory_open) {
                    log("Performance: video memory is unavailable: " + error + ".");
                }
            }
            if (memory_open) {
                if (const auto usage = memory.Read()) {
                    tick.usage_bytes = usage->first;
                    tick.budget_bytes = usage->second;
                }
            }
            tick.thread_busy_percent = thread_busy.Read(tick.frames.end_thread, start);
            if (framegen) {
                tick.framegen = framegen();
            }
            model.Tick(tick);
            {
                std::scoped_lock lock(mutex_);
                reading_ = model.Reading();
            }
            const PerformanceClock::time_point end = PerformanceClock::now();
            summary.Add(model.Reading(), {std::chrono::duration<double, std::milli>(end - start).count()});
            if (end - summary_start >= kPerformanceSummaryInterval) {
                flush(end);
            }
        } catch (const std::exception& error) {
            // A sample that failed (out of memory) is skipped; the next is tried as usual.
            gpu_state = std::string("sample failed (") + error.what() + ")";
        }
        next += kPerformanceSamplePeriod;
        if (const auto now = PerformanceClock::now(); next <= now) {
            next = now + kPerformanceSamplePeriod;
        }
    }
    flush(PerformanceClock::now());
}

} // namespace depthxr
