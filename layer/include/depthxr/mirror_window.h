#pragma once

// The stabilized mirror's desktop window (mirror.h): a plain resizable window
// in the game's process, or a borderless one that fills a chosen monitor, on
// a thread of its own with its own message loop, so nothing of it runs on a
// thread of the game's. It never takes the keyboard focus (WS_EX_NOACTIVATE,
// shown without activation), keeps its picture's aspect while it is resized,
// and closing it only hides it. Where it fills a monitor the mouse passes
// through it to whatever is under it (a game on that monitor still takes its
// clicks); a plain window takes the mouse, to be moved, resized and closed.
// The thread that ends the game's frames draws
// into it (SubmittedImageGpu::DrawMirror) and reads atomics here: no call
// waits for the window's thread but Join and Stop, which the caller makes
// holding nothing the window's thread may need (the game's Direct3D 11
// device lock above all: a swap chain being made there may wait for it).

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace depthxr {

// Where the window was left: its top-left corner on the desktop and its
// picture's size, with the width setting it was opened under (a width set
// since then in the app wins over the remembered size).
struct MirrorPlacement {
    int x{0};
    int y{0};
    int width{0};
    int height{0};
    int setting_width{0};
};
// "x,y,width,height,settingWidth"; nullopt for anything else or a size that
// is not a window's.
std::optional<MirrorPlacement> ParseMirrorPlacement(const std::wstring& text);
std::wstring FormatMirrorPlacement(const MirrorPlacement& placement);
// The remembered placement to open with: none when its title bar would be
// off the desktop (`desktop_*`: the virtual screen), and its size made the
// settings' when the width setting or the aspect changed since.
std::optional<MirrorPlacement> UsableMirrorPlacement(const std::optional<MirrorPlacement>& saved, int setting_width,
                                                     int picture_width, int picture_height, int desktop_x,
                                                     int desktop_y, int desktop_width, int desktop_height);
// Per game (its executable's name), under HKCU\Software\VectorXR\<key>.
std::optional<MirrorPlacement> LoadMirrorPlacement(const std::wstring& key, const std::string& exe_name);
void SaveMirrorPlacement(const std::wstring& key, const std::string& exe_name, const MirrorPlacement& placement);
inline constexpr wchar_t kMirrorPlacementKey[] = L"MirrorWindows";

// "VectorXR Mirror - <game>" with an en dash: what OBS's Window Capture lists.
std::wstring MirrorWindowTitle(const std::string& exe_name);
// A resize kept to the picture's aspect: the client size nearest to what the
// drag asks for (`by_width`: the drag moves a left or right edge), at least
// 160 wide.
void MirrorKeepAspect(double aspect, bool by_width, int& client_width, int& client_height);

// A monitor of the desktop: its Windows device name (the name the app's
// monitor list gives) and where it is.
struct MirrorMonitor {
    std::wstring name;
    int x{0};
    int y{0};
    int width{0};
    int height{0};
    bool primary{false};
};
std::vector<MirrorMonitor> ListMirrorMonitors();
// The settings' monitor name (UTF-8) as Windows names monitors.
std::wstring MirrorMonitorName(const std::string& name);
// The monitor of that device name (letter case aside); null for none, an
// empty name included: no monitor is ever chosen for the user.
const MirrorMonitor* FindMirrorMonitor(const std::vector<MirrorMonitor>& monitors, const std::wstring& name);

class MirrorWindow {
  public:
    struct Options {
        std::wstring title;
        int picture_width{1280};
        int picture_height{720};
        double aspect{16.0 / 9.0};
        int setting_width{1280};
        std::optional<MirrorPlacement> placement; // LoadMirrorPlacement's
        // The monitor to fill, borderless, by device name; empty for a
        // window. One that is not connected: a window, until it is.
        std::wstring fill_monitor;
        // Off: the window is made and drawn into but never put on the
        // desktop (tests).
        bool on_desktop{true};
        // Called on the window's thread when the window was moved or resized
        // and when it goes away.
        std::function<void(const MirrorPlacement&)> save;
    };

    MirrorWindow() = default;
    ~MirrorWindow();
    MirrorWindow(const MirrorWindow&) = delete;
    MirrorWindow& operator=(const MirrorWindow&) = delete;

    // Starts the thread that makes the window; returns at once. False when
    // the thread could not be started.
    bool Start(Options options, std::string& error);
    // Asks the window to go and its thread to end; returns at once. The
    // caller has let go of everything it drew into the window with. Tasks
    // posted and not run yet are run by the thread before it ends.
    void RequestStop();
    // Asked to stop, its thread not joined yet.
    bool Stopping() const { return thread_.joinable() && quit_.load(std::memory_order_acquire); }
    // Waits up to `milliseconds` (kForever: without limit) for the thread to
    // end and joins it; 0 only looks. False while it still runs. Never called
    // holding a lock the window's thread may wait for.
    static constexpr std::uint32_t kForever = 0xFFFFFFFFu;
    bool Join(std::uint32_t milliseconds);
    // RequestStop and Join(kForever).
    void Stop();
    // A thread that does not end (Join said so) is left to itself: this
    // object must then stay where it is for as long as the process runs
    // (the thread uses it), and the module that holds its code too.
    void Abandon();
    bool Started() const { return thread_.joinable(); }
    // The window, once it exists (an HWND); null before, after, and when it
    // could not be made (Failed).
    void* Handle() const { return handle_.load(std::memory_order_acquire); }
    bool Failed() const { return failed_.load(std::memory_order_acquire); }
    // Shown and not minimized, with a picture to draw into.
    bool Drawable() const;
    // The window's client size on the desktop, which a drag changes.
    void PictureSize(std::uint32_t& width, std::uint32_t& height) const;
    // The size the picture is drawn at, and the window's swap chain made at:
    // the settings' picture, or the monitor's size while it fills one. A
    // drag of the window's edge does not change it (Windows stretches the
    // picture to the window), so nothing is made anew while the window is
    // resized; only new settings or another monitor change it.
    void OutputSize(std::uint32_t& width, std::uint32_t& height) const;
    // Shown: false once the user closed it, until Show(true).
    bool Shown() const { return shown_.load(std::memory_order_acquire); }
    bool Show(bool show);
    // The settings' picture size, aspect or monitor changed: the window is
    // resized, or goes to fill the monitor, or comes back from it.
    void Reshape(int picture_width, int picture_height, double aspect, int setting_width,
                 const std::wstring& fill_monitor);
    // Runs the task on the window's thread, soon, in the order posted. False
    // when there is no window to run it (the task is not kept). A task taken
    // is always run: those still waiting when the window goes are run by its
    // thread before it ends (they hand results back to who posted them, and
    // let go of what they were given).
    bool Post(std::function<void()> task);
    // Filling its monitor now.
    bool Filling() const { return filling_.load(std::memory_order_acquire); }
    // Asked to fill a monitor that is not connected: a window meanwhile.
    bool FillMissing() const { return fill_missing_.load(std::memory_order_acquire); }

  private:
    struct Shape {
        int picture_width{1280};
        int picture_height{720};
        double aspect{16.0 / 9.0};
        int setting_width{1280};
        std::wstring fill_monitor;
    };
    void Run(Options options);
    static std::intptr_t __stdcall Procedure(void* window, unsigned message, std::uintptr_t wparam,
                                             std::intptr_t lparam);
    std::intptr_t OnMessage(void* window, unsigned message, std::uintptr_t wparam, std::intptr_t lparam);
    void Save(void* window);
    void Apply(void* window);
    void RunTasks();
    void Joined();

    std::thread thread_;
    std::atomic<void*> handle_{nullptr};
    std::atomic<bool> failed_{false};
    std::atomic<bool> shown_{false};
    std::atomic<bool> minimized_{false};
    std::atomic<bool> filling_{false};
    std::atomic<bool> fill_missing_{false};
    std::atomic<std::uint32_t> width_{0};
    std::atomic<std::uint32_t> height_{0};
    std::atomic<std::uint32_t> output_width_{0};
    std::atomic<std::uint32_t> output_height_{0};
    std::atomic<std::uint32_t> thread_id_{0};
    std::atomic<bool> quit_{false};
    // Reshape's, taken over on the window's thread. The mutex is held for a
    // copy only, never across a call into Windows.
    std::mutex pending_mutex_;
    Shape pending_;
    std::vector<std::function<void()>> tasks_; // Post's, under pending_mutex_
    // The window thread's own.
    Shape shape_;
    // The settings' picture (shape_'s is the window's, which may be a
    // remembered size).
    int setting_picture_width_{1280};
    int setting_picture_height_{720};
    bool on_desktop_{true};
    // Where the window stood before it went to fill a monitor.
    int normal_x_{0};
    int normal_y_{0};
    bool has_normal_{false};
    std::function<void(const MirrorPlacement&)> save_;
};

} // namespace depthxr
