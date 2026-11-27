#include <windows.h>

#include "depthxr/mirror_window.h"

#include <algorithm>
#include <cmath>
#include <cwchar>

#include "depthxr/process_exit.h"

namespace depthxr {
namespace {

constexpr wchar_t kClassName[] = L"VectorXRMirrorWindow";
constexpr UINT kShowMessage = WM_APP + 1;    // wparam: show
constexpr UINT kReshapeMessage = WM_APP + 2; // Reshape's pending shape
constexpr UINT kQuitMessage = WM_APP + 3;
constexpr UINT kTaskMessage = WM_APP + 4; // Post's tasks
constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX;
constexpr DWORD kFillStyle = WS_POPUP;
// Never activated; in the taskbar and in window lists all the same.
constexpr DWORD kExStyle = WS_EX_NOACTIVATE | WS_EX_APPWINDOW;
// A window that fills a monitor lets the mouse through to whatever is under
// it: it covers the whole monitor above every other window, and on the
// game's monitor it would otherwise take every click meant for the game.
// Windows passes the mouse through a window of another thread only where it
// is layered as well as transparent (WM_NCHITTEST's HTTRANSPARENT reaches
// windows of the same thread only).
constexpr DWORD kClickThroughExStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT;
constexpr int kMinPictureWidth = 160;
constexpr wchar_t kEnDash = 0x2013;

HINSTANCE ThisModule() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ThisModule), &module);
    return module ? module : GetModuleHandleW(nullptr);
}

// The window's frame around a picture of the size.
SIZE FrameAround(int width, int height) {
    RECT rect{0, 0, width, height};
    AdjustWindowRectEx(&rect, kStyle, FALSE, kExStyle);
    return SIZE{rect.right - rect.left, rect.bottom - rect.top};
}

// A plain window is left as it was: it is moved, resized and closed with the
// mouse.
void SetClickThrough(HWND window, bool on) {
    const LONG_PTR ex_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    const LONG_PTR through = static_cast<LONG_PTR>(kClickThroughExStyle);
    const LONG_PTR wanted = on ? (ex_style | through) : (ex_style & ~through);
    if (wanted == ex_style) return;
    SetWindowLongPtrW(window, GWL_EXSTYLE, wanted);
    // Opaque: a layered window shows nothing until it is told how it blends.
    if (on) SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA);
}

std::wstring Widen(const std::string& text) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(std::max(length, 0)), L'\0');
    if (length > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::wstring PlacementKey(const std::wstring& key) {
    return L"Software\\VectorXR\\" + key;
}

BOOL CALLBACK AddMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM out) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) {
        MirrorMonitor found;
        found.name = info.szDevice;
        found.x = info.rcMonitor.left;
        found.y = info.rcMonitor.top;
        found.width = info.rcMonitor.right - info.rcMonitor.left;
        found.height = info.rcMonitor.bottom - info.rcMonitor.top;
        found.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
        reinterpret_cast<std::vector<MirrorMonitor>*>(out)->push_back(std::move(found));
    }
    return TRUE;
}

} // namespace

std::optional<MirrorPlacement> ParseMirrorPlacement(const std::wstring& text) {
    MirrorPlacement placement;
    wchar_t trailing = 0;
    if (swscanf_s(text.c_str(), L"%d,%d,%d,%d,%d%c", &placement.x, &placement.y, &placement.width, &placement.height,
                  &placement.setting_width, &trailing, 1) != 5) {
        return std::nullopt;
    }
    if (placement.width < kMinPictureWidth || placement.width > 16384 || placement.height < 1 ||
        placement.height > 16384 || std::abs(placement.x) > 100000 || std::abs(placement.y) > 100000) {
        return std::nullopt;
    }
    return placement;
}

std::wstring FormatMirrorPlacement(const MirrorPlacement& placement) {
    return std::to_wstring(placement.x) + L"," + std::to_wstring(placement.y) + L"," +
           std::to_wstring(placement.width) + L"," + std::to_wstring(placement.height) + L"," +
           std::to_wstring(placement.setting_width);
}

std::optional<MirrorPlacement> UsableMirrorPlacement(const std::optional<MirrorPlacement>& saved, int setting_width,
                                                     int picture_width, int picture_height, int desktop_x,
                                                     int desktop_y, int desktop_width, int desktop_height) {
    if (!saved) return std::nullopt;
    MirrorPlacement placement = *saved;
    // Enough of the title bar on the desktop to take hold of.
    constexpr int kGrip = 80;
    if (placement.x + placement.width < desktop_x + kGrip || placement.x > desktop_x + desktop_width - kGrip ||
        placement.y < desktop_y || placement.y > desktop_y + desktop_height - kGrip) {
        return std::nullopt;
    }
    const bool same_aspect = picture_width > 0 && picture_height > 0 &&
                             std::abs(placement.width * picture_height - placement.height * picture_width) <=
                                 std::max(picture_width, picture_height);
    if (placement.setting_width != setting_width) {
        placement.width = picture_width;
        placement.height = picture_height;
    } else if (!same_aspect) {
        placement.height =
            std::max(1, static_cast<int>(std::lround(static_cast<double>(placement.width) * picture_height /
                                                     std::max(picture_width, 1))));
    }
    placement.setting_width = setting_width;
    return placement;
}

std::optional<MirrorPlacement> LoadMirrorPlacement(const std::wstring& key, const std::string& exe_name) {
    wchar_t text[96]{};
    DWORD size = sizeof(text) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, PlacementKey(key).c_str(), Widen(exe_name).c_str(), RRF_RT_REG_SZ, nullptr,
                     text, &size) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    return ParseMirrorPlacement(text);
}

void SaveMirrorPlacement(const std::wstring& key, const std::string& exe_name, const MirrorPlacement& placement) {
    const std::wstring text = FormatMirrorPlacement(placement);
    RegSetKeyValueW(HKEY_CURRENT_USER, PlacementKey(key).c_str(), Widen(exe_name).c_str(), REG_SZ, text.c_str(),
                    static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)));
}

std::wstring MirrorWindowTitle(const std::string& exe_name) {
    std::wstring name = Widen(exe_name);
    if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0) name.resize(name.size() - 4);
    std::wstring title = L"VectorXR Mirror";
    if (!name.empty()) {
        title += L' ';
        title += kEnDash;
        title += L' ';
        title += name;
    }
    return title;
}

void MirrorKeepAspect(double aspect, bool by_width, int& client_width, int& client_height) {
    if (!(aspect > 0.0)) aspect = 1.0;
    if (by_width) {
        client_width = std::max(client_width, kMinPictureWidth);
    } else {
        client_width = std::max(static_cast<int>(std::lround(client_height * aspect)), kMinPictureWidth);
    }
    client_height = std::max(1, static_cast<int>(std::lround(client_width / aspect)));
}

std::vector<MirrorMonitor> ListMirrorMonitors() {
    std::vector<MirrorMonitor> monitors;
    EnumDisplayMonitors(nullptr, nullptr, AddMonitor, reinterpret_cast<LPARAM>(&monitors));
    return monitors;
}

std::wstring MirrorMonitorName(const std::string& name) {
    return Widen(name);
}

const MirrorMonitor* FindMirrorMonitor(const std::vector<MirrorMonitor>& monitors, const std::wstring& name) {
    if (name.empty()) return nullptr;
    for (const MirrorMonitor& monitor : monitors) {
        if (_wcsicmp(monitor.name.c_str(), name.c_str()) == 0 && monitor.width > 0 && monitor.height > 0) {
            return &monitor;
        }
    }
    return nullptr;
}

MirrorWindow::~MirrorWindow() {
    if (ProcessExiting()) {
        // The thread ended with the other threads of the process: nothing
        // to wait for.
        if (thread_.joinable()) thread_.detach();
        return;
    }
    Stop();
}

bool MirrorWindow::Start(Options options, std::string& error) {
    if (thread_.joinable()) return true;
    quit_.store(false, std::memory_order_release);
    failed_.store(false, std::memory_order_release);
    // In the game's xrEndFrame: a thread Windows will not start is the
    // mirror's failure, not an exception in the game.
    try {
        thread_ = std::thread([this, options = std::move(options)]() mutable { Run(std::move(options)); });
    } catch (const std::exception& failure) {
        error = std::string("the mirror window's thread could not be started (") + failure.what() + ")";
        return false;
    } catch (...) {
        error = "the mirror window's thread could not be started";
        return false;
    }
    return true;
}

void MirrorWindow::RequestStop() {
    if (!thread_.joinable()) return;
    quit_.store(true, std::memory_order_release);
    if (HWND window = static_cast<HWND>(handle_.load(std::memory_order_acquire))) {
        // Handled inside a move or resize loop too.
        PostMessageW(window, kQuitMessage, 0, 0);
    } else if (const DWORD id = thread_id_.load(std::memory_order_acquire)) {
        PostThreadMessageW(id, WM_NULL, 0, 0);
    }
}

bool MirrorWindow::Join(std::uint32_t milliseconds) {
    if (!thread_.joinable()) return true;
    if (WaitForSingleObject(static_cast<HANDLE>(thread_.native_handle()),
                            milliseconds == kForever ? INFINITE : milliseconds) != WAIT_OBJECT_0) {
        return false;
    }
    thread_.join();
    Joined();
    return true;
}

void MirrorWindow::Stop() {
    RequestStop();
    Join(kForever);
}

void MirrorWindow::Abandon() {
    if (thread_.joinable()) thread_.detach();
}

void MirrorWindow::Joined() {
    handle_.store(nullptr, std::memory_order_release);
    shown_.store(false, std::memory_order_release);
    filling_.store(false, std::memory_order_release);
    fill_missing_.store(false, std::memory_order_release);
    width_.store(0, std::memory_order_release);
    height_.store(0, std::memory_order_release);
    output_width_.store(0, std::memory_order_release);
    output_height_.store(0, std::memory_order_release);
}

bool MirrorWindow::Drawable() const {
    return Handle() && Shown() && !minimized_.load(std::memory_order_acquire) &&
           width_.load(std::memory_order_acquire) > 0 && height_.load(std::memory_order_acquire) > 0;
}

void MirrorWindow::PictureSize(std::uint32_t& width, std::uint32_t& height) const {
    width = width_.load(std::memory_order_acquire);
    height = height_.load(std::memory_order_acquire);
}

void MirrorWindow::OutputSize(std::uint32_t& width, std::uint32_t& height) const {
    width = output_width_.load(std::memory_order_acquire);
    height = output_height_.load(std::memory_order_acquire);
}

bool MirrorWindow::Show(bool show) {
    const HWND window = static_cast<HWND>(Handle());
    return window && PostMessageW(window, kShowMessage, show ? 1 : 0, 0);
}

void MirrorWindow::Reshape(int picture_width, int picture_height, double aspect, int setting_width,
                           const std::wstring& fill_monitor) {
    {
        std::scoped_lock lock(pending_mutex_);
        pending_.picture_width = picture_width;
        pending_.picture_height = picture_height;
        pending_.aspect = aspect;
        pending_.setting_width = setting_width;
        pending_.fill_monitor = fill_monitor;
    }
    if (HWND window = static_cast<HWND>(Handle())) PostMessageW(window, kReshapeMessage, 0, 0);
}

bool MirrorWindow::Post(std::function<void()> task) {
    std::scoped_lock lock(pending_mutex_);
    const HWND window = static_cast<HWND>(Handle());
    if (!window) return false;
    tasks_.push_back(std::move(task));
    // Posting never waits for the window's thread.
    if (!PostMessageW(window, kTaskMessage, 0, 0)) {
        tasks_.pop_back();
        return false;
    }
    return true;
}

void MirrorWindow::Save(void* handle) {
    const HWND window = static_cast<HWND>(handle);
    // Where it fills a monitor is not where the window is to be remembered.
    if (!save_ || IsIconic(window) || filling_.load(std::memory_order_acquire)) return;
    RECT frame{};
    RECT client{};
    if (!GetWindowRect(window, &frame) || !GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0) {
        return;
    }
    save_(MirrorPlacement{frame.left, frame.top, client.right, client.bottom, shape_.setting_width});
}

// The window as `shape_` asks: filling its monitor where that is connected,
// else a window of the picture's size.
void MirrorWindow::Apply(void* handle) {
    const HWND window = static_cast<HWND>(handle);
    const std::vector<MirrorMonitor> monitors = ListMirrorMonitors();
    const MirrorMonitor* monitor = FindMirrorMonitor(monitors, shape_.fill_monitor);
    const bool was_filling = filling_.load(std::memory_order_acquire);
    const LONG_PTR visible = GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE;
    if (IsIconic(window)) ShowWindow(window, SW_SHOWNOACTIVATE);
    if (monitor) {
        if (!was_filling) {
            RECT frame{};
            if (GetWindowRect(window, &frame)) {
                normal_x_ = frame.left;
                normal_y_ = frame.top;
                has_normal_ = true;
            }
            SetWindowLongPtrW(window, GWL_STYLE, static_cast<LONG_PTR>(kFillStyle) | visible);
        }
        SetClickThrough(window, true);
        filling_.store(true, std::memory_order_release);
        // Above the taskbar of its monitor, without being activated.
        SetWindowPos(window, HWND_TOPMOST, monitor->x, monitor->y, monitor->width, monitor->height,
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
    } else {
        const SIZE frame = FrameAround(shape_.picture_width, shape_.picture_height);
        SetClickThrough(window, false);
        if (was_filling) {
            SetWindowLongPtrW(window, GWL_STYLE, static_cast<LONG_PTR>(kStyle) | visible);
            filling_.store(false, std::memory_order_release);
            SetWindowPos(window, HWND_NOTOPMOST, normal_x_, normal_y_, frame.cx, frame.cy,
                         SWP_NOACTIVATE | SWP_FRAMECHANGED | (has_normal_ ? 0u : SWP_NOMOVE));
        } else {
            SetWindowPos(window, nullptr, 0, 0, frame.cx, frame.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    fill_missing_.store(!monitor && !shape_.fill_monitor.empty(), std::memory_order_release);
    // What the picture is drawn at: the monitor's pixels, or the settings'.
    output_width_.store(static_cast<std::uint32_t>(monitor ? monitor->width : setting_picture_width_),
                        std::memory_order_release);
    output_height_.store(static_cast<std::uint32_t>(monitor ? monitor->height : setting_picture_height_),
                         std::memory_order_release);
}

std::intptr_t __stdcall MirrorWindow::Procedure(void* handle, unsigned message, std::uintptr_t wparam,
                                                std::intptr_t lparam) {
    const HWND window = static_cast<HWND>(handle);
    if (message == WM_NCCREATE) {
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<const CREATESTRUCTW*>(lparam)->lpCreateParams));
    }
    if (auto* self = reinterpret_cast<MirrorWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA))) {
        return self->OnMessage(handle, message, wparam, lparam);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

std::intptr_t MirrorWindow::OnMessage(void* handle, unsigned message, std::uintptr_t wparam, std::intptr_t lparam) {
    const HWND window = static_cast<HWND>(handle);
    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_CLOSE:
        // Closing hides: the binding or the setting shows it again.
        Save(handle);
        ShowWindow(window, SW_HIDE);
        shown_.store(false, std::memory_order_release);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        ValidateRect(window, nullptr);
        return 0;
    case WM_SIZE:
        minimized_.store(wparam == SIZE_MINIMIZED, std::memory_order_release);
        if (wparam != SIZE_MINIMIZED) {
            width_.store(LOWORD(lparam), std::memory_order_release);
            height_.store(HIWORD(lparam), std::memory_order_release);
        }
        return 0;
    case WM_SIZING: {
        if (filling_.load(std::memory_order_acquire)) return TRUE;
        auto* rect = reinterpret_cast<RECT*>(lparam);
        const SIZE frame = FrameAround(0, 0);
        int width = rect->right - rect->left - frame.cx;
        int height = rect->bottom - rect->top - frame.cy;
        const bool by_width = wparam != WMSZ_TOP && wparam != WMSZ_BOTTOM;
        MirrorKeepAspect(shape_.aspect, by_width, width, height);
        // The edge being dragged moves; the one across from it stays.
        if (wparam == WMSZ_LEFT || wparam == WMSZ_TOPLEFT || wparam == WMSZ_BOTTOMLEFT) {
            rect->left = rect->right - width - frame.cx;
        } else {
            rect->right = rect->left + width + frame.cx;
        }
        if (wparam == WMSZ_TOP || wparam == WMSZ_TOPLEFT || wparam == WMSZ_TOPRIGHT) {
            rect->top = rect->bottom - height - frame.cy;
        } else {
            rect->bottom = rect->top + height + frame.cy;
        }
        return TRUE;
    }
    case WM_GETMINMAXINFO: {
        if (filling_.load(std::memory_order_acquire)) return DefWindowProcW(window, message, wparam, lparam);
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        int width = kMinPictureWidth;
        int height = 0;
        MirrorKeepAspect(shape_.aspect, true, width, height);
        const SIZE frame = FrameAround(width, height);
        info->ptMinTrackSize = POINT{frame.cx, frame.cy};
        return 0;
    }
    case WM_EXITSIZEMOVE:
        Save(handle);
        return 0;
    case WM_DISPLAYCHANGE:
        // A monitor came or went, or changed its size: the one to fill may
        // be there now, or gone.
        if (!shape_.fill_monitor.empty()) Apply(handle);
        return 0;
    case kShowMessage:
        if (wparam) {
            if (on_desktop_) ShowWindow(window, IsIconic(window) ? SW_SHOWNOACTIVATE : SW_SHOWNA);
        } else {
            Save(handle);
            ShowWindow(window, SW_HIDE);
        }
        shown_.store(wparam != 0, std::memory_order_release);
        return 0;
    case kReshapeMessage: {
        {
            std::scoped_lock lock(pending_mutex_);
            shape_ = pending_;
        }
        setting_picture_width_ = shape_.picture_width;
        setting_picture_height_ = shape_.picture_height;
        Apply(handle);
        Save(handle);
        return 0;
    }
    case kTaskMessage:
        RunTasks();
        return 0;
    case kQuitMessage:
        Save(handle);
        DestroyWindow(window);
        return 0;
    case WM_DESTROY: {
        // No task is taken from here on (Post, under the same mutex); those
        // taken are run once the window is gone (Run).
        std::scoped_lock lock(pending_mutex_);
        handle_.store(nullptr, std::memory_order_release);
        shown_.store(false, std::memory_order_release);
        return 0;
    }
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

void MirrorWindow::RunTasks() {
    for (;;) {
        std::vector<std::function<void()>> tasks;
        {
            std::scoped_lock lock(pending_mutex_);
            tasks.swap(tasks_);
        }
        if (tasks.empty()) return;
        for (std::function<void()>& task : tasks) {
            // A task that throws must not end the game with the thread.
            try {
                task();
            } catch (...) {
            }
        }
    }
}

void MirrorWindow::Run(Options options) {
    thread_id_.store(GetCurrentThreadId(), std::memory_order_release);
    shape_.picture_width = options.picture_width;
    shape_.picture_height = options.picture_height;
    shape_.aspect = options.aspect;
    shape_.setting_width = options.setting_width;
    shape_.fill_monitor = options.fill_monitor;
    {
        std::scoped_lock lock(pending_mutex_);
        pending_ = shape_;
    }
    setting_picture_width_ = options.picture_width;
    setting_picture_height_ = options.picture_height;
    on_desktop_ = options.on_desktop;
    has_normal_ = false;
    save_ = std::move(options.save);
    // Its own pixels on a scaled display, whatever the game told Windows of
    // itself (Windows 10 1703 and later; without, as the game is).
    if (const HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        using SetContext = void*(WINAPI*)(void*);
        if (const auto set = reinterpret_cast<SetContext>(GetProcAddress(user32, "SetThreadDpiAwarenessContext"))) {
            set(reinterpret_cast<void*>(static_cast<INT_PTR>(-4))); // per monitor, version 2
        }
    }
    const HINSTANCE module = ThisModule();
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = reinterpret_cast<WNDPROC>(&MirrorWindow::Procedure);
    window_class.hInstance = module;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // the arrow
    window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    window_class.lpszClassName = kClassName;
    const bool registered = RegisterClassExW(&window_class) != 0;
    HWND window = nullptr;
    if (registered || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        const std::optional<MirrorPlacement> placement = UsableMirrorPlacement(
            options.placement, options.setting_width, options.picture_width, options.picture_height,
            GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
            GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
        if (placement) {
            shape_.picture_width = placement->width;
            shape_.picture_height = placement->height;
        }
        const SIZE frame = FrameAround(shape_.picture_width, shape_.picture_height);
        window = CreateWindowExW(kExStyle, kClassName, options.title.c_str(), kStyle,
                                 placement ? placement->x : CW_USEDEFAULT, placement ? placement->y : CW_USEDEFAULT,
                                 frame.cx, frame.cy, nullptr, nullptr, module, this);
    }
    if (!window) {
        failed_.store(true, std::memory_order_release);
        if (registered) UnregisterClassW(kClassName, module);
        return;
    }
    output_width_.store(static_cast<std::uint32_t>(setting_picture_width_), std::memory_order_release);
    output_height_.store(static_cast<std::uint32_t>(setting_picture_height_), std::memory_order_release);
    if (!shape_.fill_monitor.empty()) Apply(window);
    RECT client{};
    GetClientRect(window, &client);
    width_.store(static_cast<std::uint32_t>(client.right), std::memory_order_release);
    height_.store(static_cast<std::uint32_t>(client.bottom), std::memory_order_release);
    // Without activation: the game keeps the keyboard, and stays in front
    // where it is full screen.
    if (on_desktop_) ShowWindow(window, SW_SHOWNOACTIVATE);
    shown_.store(true, std::memory_order_release);
    handle_.store(window, std::memory_order_release);
    for (;;) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!IsWindow(window)) break;
        if (quit_.load(std::memory_order_acquire)) {
            DestroyWindow(window);
            break;
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    {
        std::scoped_lock lock(pending_mutex_);
        handle_.store(nullptr, std::memory_order_release);
    }
    // Every task taken is run, the window gone or not: whoever posted one
    // waits for what it hands back, and it lets go of what it was given.
    RunTasks();
    shown_.store(false, std::memory_order_release);
    if (registered) UnregisterClassW(kClassName, module);
}

} // namespace depthxr
