#pragma once

// Included by .cpp files only: it brings in Direct3D 11's headers.
#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <utility>

namespace depthxr {

// The game's Direct3D 11 immediate context's lock (ID3D11Multithread), held
// by GameContextLock::Hold until the hold goes.
class GameContextHold {
  public:
    GameContextHold() = default;
    explicit GameContextHold(Microsoft::WRL::ComPtr<ID3D11Multithread> entered) : entered_(std::move(entered)) {}
    GameContextHold(GameContextHold&& other) noexcept : entered_(std::move(other.entered_)) {}
    GameContextHold& operator=(GameContextHold&& other) noexcept {
        if (this != &other) {
            Release();
            entered_ = std::move(other.entered_);
        }
        return *this;
    }
    GameContextHold(const GameContextHold&) = delete;
    GameContextHold& operator=(const GameContextHold&) = delete;
    ~GameContextHold() { Release(); }

    bool Held() const { return entered_ != nullptr; }
    // Lets the lock go now (on the thread that took it); again does nothing.
    void Release() {
        if (entered_) {
            entered_->Leave();
            entered_.Reset();
        }
    }

  private:
    Microsoft::WRL::ComPtr<ID3D11Multithread> entered_;
};

// The lock order for the game's Direct3D 11 immediate context: the device's
// lock (ID3D11Multithread::Enter) before any lock of the layer's.
//
// With multithread protection on, every call on the immediate context takes
// the device's lock (Flush and a few others take it even without). A game
// may hold that lock (Enter) on one thread through an OpenXR call, which
// waits for the layer's locks; a layer thread that waited for the device's
// lock while holding one of them would deadlock with it. So code that calls
// the game's context under its own locks takes the device's lock first,
// holding none of them: Hold, before the first of its locks, kept until the
// last of its calls on the context is made. Enter is recursive: calls on the
// context, and holds taken again inside, then never wait.
//
// Only while the game's device is multithread protected: without protection
// the context's calls take no lock a game could hold across its OpenXR calls
// (but for Flush), and the layer adds no Enter/Leave to them. Protection is
// read at each Hold (holding no lock: Direct3D 11's debug layer waits for
// the device's lock to read it) and never changed here.
//
// Set and Hold may be called from any thread.
class GameContextLock {
  public:
    // The game's device (its immediate context's lock), or none.
    void Set(ID3D11Device* device) {
        Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
        if (device) {
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
            device->GetImmediateContext(&context);
            if (context) {
                context.As(&multithread);
            }
        }
        std::scoped_lock lock(mutex_);
        multithread_ = std::move(multithread);
        active_.store(multithread_ != nullptr, std::memory_order_release);
    }
    bool Active() const { return active_.load(std::memory_order_acquire); }

    // Before taking any lock of the caller's: enters the device's lock when
    // its multithread protection is on, else holds nothing.
    GameContextHold Hold() {
        if (!Active()) {
            return {};
        }
        Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
        std::function<void()> hook;
        {
            std::scoped_lock lock(mutex_);
            multithread = multithread_;
            if (has_hook_) hook = wait_hook_;
        }
        if (!multithread) {
            return {};
        }
        if (hook) {
            hook();
        }
        if (multithread->GetMultithreadProtected() == FALSE) {
            return {};
        }
        multithread->Enter();
        return GameContextHold(std::move(multithread));
    }

    // Tests: called in Hold, on its thread, before it reads the device's
    // protection and enters its lock (holding none of the caller's locks).
    void SetWaitHookForTest(std::function<void()> hook) {
        std::scoped_lock lock(mutex_);
        has_hook_ = static_cast<bool>(hook);
        wait_hook_ = std::move(hook);
    }

  private:
    mutable std::mutex mutex_; // only while copying the members: no call is made under it
    Microsoft::WRL::ComPtr<ID3D11Multithread> multithread_;
    std::atomic<bool> active_{false};
    bool has_hook_{false};
    std::function<void()> wait_hook_;
};

} // namespace depthxr
