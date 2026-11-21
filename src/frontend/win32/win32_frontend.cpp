// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/win32/win32_frontend.hpp"

#include "app/application.hpp"
#include "app/host_link.hpp"
#include "config/profile.hpp"
#include "frontend/win32/resource.h"
#include "frontend/win32/win32_dialogs.hpp"
#include "frontend/win32/win32_util.hpp"
#include "input/host_keymap.hpp"
#include "machine/emulator.hpp"
#include "models/model.hpp"
#include "present/display_profile.hpp"
#include "util/log.hpp"
#include "util/strings.hpp"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <set>

namespace em34xx {

namespace {

using namespace win32;
using steady = std::chrono::steady_clock;

constexpr UINT WM_APP_DISPLAY = WM_APP + 10;   // a new picture may be ready
constexpr UINT WM_APP_TITLE = WM_APP + 11;     // set_title / set_status
constexpr UINT WM_APP_QUIT = WM_APP + 12;      // request_quit
constexpr UINT WM_APP_TRUST = WM_APP + 13;     // a TLS trust decision is wanted
constexpr UINT WM_APP_HOLD = WM_APP + 14;      // close one station's session (wParam)
#ifndef WM_DPICHANGED
constexpr UINT WM_DPICHANGED = 0x02E0;
#endif

constexpr UINT_PTR kTimerRaster = 1;
constexpr UINT_PTR kTimerStatus = 2;
constexpr auto kMinRasterInterval = std::chrono::milliseconds(15);

const wchar_t kWindowClass[] = L"em34xxw.Terminal";

// --- TLS trust requests: I/O threads wait, the UI thread answers ------------

class TrustQueue {
public:
    struct Pending {
        tn5250::TlsTrustRequest req;
        bool done = false;
        tn5250::TlsTrustDecision decision = tn5250::TlsTrustDecision::Reject;
    };

    void attach(HWND hwnd) {
        std::lock_guard<std::mutex> g(mu_);
        hwnd_ = hwnd;
    }

    // On a session's I/O thread.
    tn5250::TlsTrustDecision ask(const tn5250::TlsTrustRequest& req) {
        std::unique_lock<std::mutex> lock(mu_);
        if (closed_ || !hwnd_) return tn5250::TlsTrustDecision::Reject;
        auto p = std::make_shared<Pending>();
        p->req = req;
        queue_.push_back(p);
        PostMessageW(hwnd_, WM_APP_TRUST, 0, 0);
        // The server may drop an idle handshake; ten minutes is plenty.
        cv_.wait_for(lock, std::chrono::minutes(10), [&] { return p->done || closed_; });
        if (!p->done) {
            queue_.erase(std::remove(queue_.begin(), queue_.end(), p), queue_.end());
            return tn5250::TlsTrustDecision::Reject;
        }
        return p->decision;
    }

    std::shared_ptr<Pending> take() {
        std::lock_guard<std::mutex> g(mu_);
        if (queue_.empty()) return nullptr;
        auto p = queue_.front();
        queue_.pop_front();
        return p;
    }

    void answer(const std::shared_ptr<Pending>& p, tn5250::TlsTrustDecision d) {
        std::lock_guard<std::mutex> g(mu_);
        p->decision = d;
        p->done = true;
        cv_.notify_all();
    }

    // Reject everything still waiting (before a disconnect: the sessions'
    // threads are joined, so none may stay blocked here).
    void cancel_all() {
        std::lock_guard<std::mutex> g(mu_);
        for (auto& p : queue_) p->done = true;
        queue_.clear();
        cv_.notify_all();
    }

    void close() {
        std::lock_guard<std::mutex> g(mu_);
        closed_ = true;
        hwnd_ = nullptr;
        for (auto& p : queue_) p->done = true;
        queue_.clear();
        cv_.notify_all();
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<Pending>> queue_;
    HWND hwnd_ = nullptr;
    bool closed_ = false;
};

// The profile the window is connected with (or last was).
struct CurrentProfile {
    bool active = false;
    Profile p;
    std::string name;      // file stem; "" when untitled
    std::string path;
    bool untitled = false;
    bool modified = false;
    bool accepted_once = false;   // a certificate was accepted for this connection only
};

// Toolbar: glyph index in the IDB_TOOLBAR* strips, command, string.
struct ToolButton {
    int image;   // -1: separator; -2: text only
    int command;
    UINT text;
};
const ToolButton kToolbar[] = {
    {0, ID_FILE_PROFILES, IDS_TB_PROFILES}, {1, ID_FILE_CONNECT, IDS_TB_CONNECT}, {2, ID_FILE_SAVE, IDS_TB_SAVE},
    {-1, 0, 0},                             {3, ID_FILE_DISCONNECT, IDS_TB_DISCONNECT},
    {-1, 0, 0},                             {4, ID_EDIT_SETTINGS, IDS_TB_SETTINGS},
    {5, ID_EDIT_KEYMAP, IDS_TB_KEYMAP},     {-1, 0, 0},
    {-2, ID_TERM_ATTN, IDS_TB_ATTN},        {-2, ID_TERM_SYSRQ, IDS_TB_SYSRQ},
    {-2, ID_TERM_RESET, IDS_TB_RESET},
};

class Win32Frontend : public Frontend {
public:
    explicit Win32Frontend(Win32Launch& launch) : launch_(launch), trust_(std::make_shared<TrustQueue>()) {}

    const char* name() const override { return "win32"; }
    bool run(Application& app, const FrontendOptions& opts, std::string& error) override;
    void request_quit() override {
        quit_.store(true);
        HWND h = hwnd_.load();
        if (h) PostMessageW(h, WM_APP_QUIT, 0, 0);
    }
    void set_title(const std::string& title) override {
        {
            std::lock_guard<std::mutex> g(text_mu_);
            title_ = title;
        }
        HWND h = hwnd_.load();
        if (h) PostMessageW(h, WM_APP_TITLE, 0, 0);
    }
    void set_status(const std::string& status) override {
        {
            std::lock_guard<std::mutex> g(text_mu_);
            status_note_ = status;
        }
        HWND h = hwnd_.load();
        if (h) PostMessageW(h, WM_APP_TITLE, 0, 0);
    }

private:
    static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    bool create_window(std::string& error);
    void create_bars();
    void build_toolbar_images();
    void layout();
    RECT display_rect() const;
    void size_for_zoom(bool keep_position);
    void restore_placement();
    void save_placement();

    void schedule_render();
    void render_now();
    void paint();

    void key(UINT msg, WPARAM wp, LPARAM lp);
    void release_keys();
    void terminal_key(const char* function);

    void command(int id);
    void update_title();
    void update_status();
    void update_ui_state();

    void connect_profile(const Profile& p);
    void open_profile_file(const std::string& path);
    bool save_current(bool save_as);
    bool confirm_discard();
    void edit_keymap();
    void edit_settings();
    void handle_trust_requests();
    void disconnect();
    std::string profile_display_name() const;

    Win32Launch& launch_;
    AppSettings& settings() { return launch_.settings; }
    const UserDirs& dirs() const { return *launch_.dirs; }

    Application* app_ = nullptr;
    Emulator* emu_ = nullptr;
    std::string model_name_;
    std::atomic<HWND> hwnd_{nullptr};
    HWND toolbar_ = nullptr;
    HWND status_ = nullptr;
    HACCEL accel_ = nullptr;
    HIMAGELIST images_ = nullptr;
    HFONT font_ = nullptr;
    UINT dpi_ = 96;

    Framebuffer fb_;
    CrtRenderer crt_;
    CrtParams crt_params_;
    std::vector<uint32_t> image_;
    int img_w_ = 0, img_h_ = 0;
    RECT img_rect_{};     // where the image is in the client area
    int base_w_ = 640, base_h_ = 480;   // presented frame size (aspect-corrected)
    bool dirty_ = true;
    bool raster_timer_ = false;
    steady::time_point last_raster_{};
    std::atomic<bool> display_pending_{false};
    std::atomic<bool> quit_{false};

    std::set<uint32_t> held_;   // HID usages held, sent to the terminal

    std::mutex text_mu_;
    std::string title_, status_note_;

    std::shared_ptr<TrustQueue> trust_;
    bool showing_trust_ = false;
    CurrentProfile cur_;
    std::string last_status_[4];
};

// ---------------------------------------------------------------------------
// Window

LRESULT CALLBACK Win32Frontend::wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Win32Frontend* self = reinterpret_cast<Win32Frontend*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
        self = static_cast<Win32Frontend*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        // Per-monitor v1 (Windows 8.1 / early 10): scale the non-client area.
        using Fn = BOOL(WINAPI*)(HWND);
        if (auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(
                GetProcAddress(GetModuleHandleW(L"user32.dll"), "EnableNonClientDpiScaling"))))
            fn(hwnd);
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->handle(hwnd, msg, wp, lp);
}

bool Win32Frontend::create_window(std::string& error) {
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = launch_.instance;
    wc.hIcon = LoadIconW(launch_.instance, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = static_cast<HICON>(LoadImageW(launch_.instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;   // the picture area is painted, black borders included
    wc.lpszMenuName = MAKEINTRESOURCEW(IDR_MAINMENU);
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        error = "cannot register the window class";
        return false;
    }
    HWND h = CreateWindowExW(0, kWindowClass, load_string(IDS_APP_TITLE).c_str(),
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, nullptr,
                             nullptr, launch_.instance, this);
    if (!h) {
        error = format("cannot create the main window (error %lu)", static_cast<unsigned long>(GetLastError()));
        return false;
    }
    hwnd_.store(h);
    dpi_ = window_dpi(h);
    accel_ = LoadAcceleratorsW(launch_.instance, MAKEINTRESOURCEW(IDR_ACCEL));
    create_bars();
    return true;
}

void Win32Frontend::build_toolbar_images() {
    const int size = dpi_ >= 168 ? 32 : dpi_ >= 120 ? 24 : 16;
    const int id = size == 32 ? IDB_TOOLBAR32 : size == 24 ? IDB_TOOLBAR24 : IDB_TOOLBAR16;
    HBITMAP bmp = static_cast<HBITMAP>(
        LoadImageW(launch_.instance, MAKEINTRESOURCEW(id), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
    HIMAGELIST il = ImageList_Create(size, size, ILC_COLOR32, 6, 0);
    if (bmp && il) ImageList_Add(il, bmp, nullptr);
    if (bmp) DeleteObject(bmp);
    SendMessageW(toolbar_, TB_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(il));
    if (images_) ImageList_Destroy(images_);
    images_ = il;
    HFONT f = create_ui_font(dpi_);
    SendMessageW(toolbar_, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
    SendMessageW(status_, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
    if (font_) DeleteObject(font_);
    font_ = f;
    SendMessageW(toolbar_, TB_AUTOSIZE, 0, 0);
}

void Win32Frontend::create_bars() {
    HWND h = hwnd_.load();
    toolbar_ = CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
                               WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | TBSTYLE_LIST | CCS_TOP, 0, 0,
                               0, 0, h, nullptr, launch_.instance, nullptr);
    SendMessageW(toolbar_, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    SendMessageW(toolbar_, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_DOUBLEBUFFER);
    status_ = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr, WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, h,
                              nullptr, launch_.instance, nullptr);
    build_toolbar_images();
    static std::vector<std::wstring> texts;   // TB_ADDBUTTONS copies the strings
    texts.clear();
    for (const ToolButton& b : kToolbar) texts.push_back(b.text ? load_string(b.text) : std::wstring());
    std::vector<TBBUTTON> buttons;
    for (size_t i = 0; i < sizeof kToolbar / sizeof kToolbar[0]; ++i) {
        const ToolButton& b = kToolbar[i];
        TBBUTTON t;
        ZeroMemory(&t, sizeof t);
        if (b.image == -1) {
            t.fsStyle = BTNS_SEP;
        } else {
            t.iBitmap = b.image >= 0 ? b.image : I_IMAGENONE;
            t.idCommand = b.command;
            t.fsState = TBSTATE_ENABLED;
            t.fsStyle = BTNS_BUTTON | BTNS_AUTOSIZE;
            // Attn, SysRq and Reset show their names; the others are
            // pictures with tooltips (TTN_GETDISPINFO).
            t.iString = b.image == -2 ? reinterpret_cast<INT_PTR>(texts[i].c_str()) : INT_PTR(-1);
        }
        buttons.push_back(t);
    }
    SendMessageW(toolbar_, TB_ADDBUTTONS, WPARAM(buttons.size()), reinterpret_cast<LPARAM>(buttons.data()));
    SendMessageW(toolbar_, TB_AUTOSIZE, 0, 0);
}

void Win32Frontend::layout() {
    HWND h = hwnd_.load();
    SendMessageW(toolbar_, TB_AUTOSIZE, 0, 0);
    SendMessageW(status_, WM_SIZE, 0, 0);
    RECT sr;
    GetClientRect(status_, &sr);
    const int w = sr.right;
    const int parts[4] = {scale_px(200, dpi_) < w ? scale_px(200, dpi_) : w / 4,
                          scale_px(400, dpi_) < w ? scale_px(400, dpi_) : w / 2,
                          scale_px(560, dpi_) < w ? scale_px(560, dpi_) : 3 * w / 4, -1};
    SendMessageW(status_, SB_SETPARTS, 4, reinterpret_cast<LPARAM>(parts));
    (void)h;
    dirty_ = true;
    schedule_render();
}

RECT Win32Frontend::display_rect() const {
    RECT c;
    GetClientRect(hwnd_.load(), &c);
    RECT tr, sr;
    GetWindowRect(toolbar_, &tr);
    GetWindowRect(status_, &sr);
    c.top += tr.bottom - tr.top;
    c.bottom -= sr.bottom - sr.top;
    if (c.bottom < c.top) c.bottom = c.top;
    return c;
}

// Client size: the presented frame times the zoom times the DPI scale, plus
// the bars.
void Win32Frontend::size_for_zoom(bool keep_position) {
    HWND h = hwnd_.load();
    if (IsZoomed(h)) ShowWindow(h, SW_RESTORE);
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
    const DWORD style = DWORD(GetWindowLongW(h, GWL_STYLE)), ex_style = DWORD(GetWindowLongW(h, GWL_EXSTYLE));
    RECT tr, sr;
    GetWindowRect(toolbar_, &tr);
    GetWindowRect(status_, &sr);
    const int bars = (tr.bottom - tr.top) + (sr.bottom - sr.top);
    double scale = settings().zoom * double(dpi_) / 96.0;
    RECT r{};
    for (int attempt = 0; attempt < 2; ++attempt) {
        const int dw = int(base_w_ * scale + 0.5), dh = int(base_h_ * scale + 0.5);
        r = RECT{0, 0, dw, dh + bars};
        adjust_window_rect(r, style, TRUE, ex_style, dpi_);
        // Larger than the work area: the largest size that fits, same aspect.
        const double fit = std::min(double(mi.rcWork.right - mi.rcWork.left) / double(r.right - r.left),
                                    double(mi.rcWork.bottom - mi.rcWork.top) / double(r.bottom - r.top));
        if (fit >= 1.0) break;
        scale *= fit * 0.98;
    }
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    RECT wr;
    GetWindowRect(h, &wr);
    int x = wr.left, y = wr.top;
    if (!keep_position || x + ww > mi.rcWork.right) x = std::max(int(mi.rcWork.left), int(mi.rcWork.right) - ww);
    if (!keep_position || y + wh > mi.rcWork.bottom) y = std::max(int(mi.rcWork.top), int(mi.rcWork.bottom) - wh);
    if (keep_position) {
        x = std::max(x, int(mi.rcWork.left));
        y = std::max(y, int(mi.rcWork.top));
    }
    SetWindowPos(h, nullptr, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
}

void Win32Frontend::restore_placement() {
    HWND h = hwnd_.load();
    const WindowPlacement& wp = settings().window;
    if (wp.valid) {
        RECT r{wp.x, wp.y, wp.x + wp.width, wp.y + wp.height};
        if (HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
            MONITORINFO mi;
            mi.cbSize = sizeof mi;
            GetMonitorInfoW(mon, &mi);
            const RECT& w = mi.rcWork;
            // Keep it on that monitor's work area (resolution or DPI may
            // have changed since).
            if (wp.width <= w.right - w.left && wp.height <= w.bottom - w.top) {
                const int x = std::max(int(w.left), std::min(int(r.left), int(w.right) - wp.width));
                const int y = std::max(int(w.top), std::min(int(r.top), int(w.bottom) - wp.height));
                SetWindowPos(h, nullptr, x, y, wp.width, wp.height, SWP_NOZORDER | SWP_NOACTIVATE);
                dpi_ = window_dpi(h);
                return;
            }
        }
    }
    size_for_zoom(true);
}

void Win32Frontend::save_placement() {
    WINDOWPLACEMENT wp;
    wp.length = sizeof wp;
    if (!GetWindowPlacement(hwnd_.load(), &wp)) return;
    WindowPlacement& s = settings().window;
    s.valid = true;
    s.x = wp.rcNormalPosition.left;
    s.y = wp.rcNormalPosition.top;
    s.width = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
    s.height = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
    s.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
}

// ---------------------------------------------------------------------------
// Picture

void Win32Frontend::schedule_render() {
    if (!dirty_) return;
    const auto now = steady::now();
    const auto due = last_raster_ + kMinRasterInterval;
    if (now >= due) {
        render_now();
        return;
    }
    if (!raster_timer_) {
        raster_timer_ = true;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(due - now).count() + 1;
        SetTimer(hwnd_.load(), kTimerRaster, UINT(ms), nullptr);
    }
}

void Win32Frontend::render_now() {
    HWND h = hwnd_.load();
    dirty_ = false;
    last_raster_ = steady::now();
    emu_->with_machine([&](Machine& m) { render_presented(m, fb_); });
    const RECT d = display_rect();
    const int dw = std::max(1, int(d.right - d.left)), dh = std::max(1, int(d.bottom - d.top));
    int out_w = 1, out_h = 1;
    crt_fit(fb_, dw, dh, out_w, out_h);
    const bool resized = out_w != img_w_ || out_h != img_h_;
    if (resized) {
        img_w_ = out_w;
        img_h_ = out_h;
        crt_.invalidate();
    }
    const RECT where{d.left + (dw - out_w) / 2, d.top + (dh - out_h) / 2, d.left + (dw - out_w) / 2 + out_w,
                     d.top + (dh - out_h) / 2 + out_h};
    const bool moved = !EqualRect(&where, &img_rect_);
    img_rect_ = where;
    const CrtRenderer::Rows rows = crt_.render(fb_, out_w, out_h, crt_params_, image_);
    if (resized || moved) {
        InvalidateRect(h, &d, FALSE);
    } else if (!rows.empty()) {
        RECT r{where.left, where.top + rows.row0, where.right, where.top + rows.row1};
        InvalidateRect(h, &r, FALSE);
    }
}

void Win32Frontend::paint() {
    HWND h = hwnd_.load();
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    const RECT d = display_rect();
    // Black around the picture.
    HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RECT parts[4] = {{d.left, d.top, d.right, img_rect_.top},
                     {d.left, img_rect_.bottom, d.right, d.bottom},
                     {d.left, img_rect_.top, img_rect_.left, img_rect_.bottom},
                     {img_rect_.right, img_rect_.top, d.right, img_rect_.bottom}};
    for (RECT& r : parts)
        if (r.right > r.left && r.bottom > r.top) FillRect(dc, &r, black);
    if (img_w_ > 0 && img_h_ > 0 && image_.size() >= size_t(img_w_) * size_t(img_h_)) {
        BITMAPINFO bi;
        ZeroMemory(&bi, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = img_w_;
        bi.bmiHeader.biHeight = -img_h_;   // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetDIBitsToDevice(dc, img_rect_.left, img_rect_.top, DWORD(img_w_), DWORD(img_h_), 0, 0, 0, UINT(img_h_),
                          image_.data(), &bi, DIB_RGB_COLORS);
    } else {
        FillRect(dc, &d, black);
    }
    EndPaint(h, &ps);
}

// ---------------------------------------------------------------------------
// Keyboard

void Win32Frontend::key(UINT msg, WPARAM wp, LPARAM lp) {
    const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    if (down && (lp & (LPARAM(1) << 30))) return;   // host auto-repeat: the terminal does its own
    // AltGr arrives as a fake Left Ctrl followed by Right Alt with the same
    // time stamp: drop the fake Ctrl.
    if (down && wp == VK_CONTROL && !(lp & (LPARAM(1) << 24))) {
        MSG next;
        if (PeekMessageW(&next, hwnd_.load(), WM_KEYDOWN, WM_SYSKEYDOWN, PM_NOREMOVE) && next.wParam == VK_MENU &&
            (next.lParam & (LPARAM(1) << 24)) && next.time == DWORD(GetMessageTime()))
            return;
    }
    const uint32_t hid = hid_from_key_message(wp, lp);
    if (!hid) return;
    if (down) {
        if (!held_.insert(hid).second) return;
    } else if (!held_.erase(hid)) {
        return;   // never sent down (AltGr's fake Ctrl, a key pressed before focus)
    }
    HostKeyEvent ev;
    ev.hid_usage = hid;
    ev.pressed = down;
    ev.name = host_key_name(hid);
    emu_->with_machine([&](Machine& m) { m.key_event(ev); });
}

void Win32Frontend::release_keys() {
    if (held_.empty()) return;
    const std::set<uint32_t> held = held_;
    held_.clear();
    emu_->with_machine([&](Machine& m) {
        for (uint32_t hid : held) {
            HostKeyEvent ev;
            ev.hid_usage = hid;
            ev.pressed = false;
            ev.name = host_key_name(hid);
            m.key_event(ev);
        }
    });
}

void Win32Frontend::terminal_key(const char* function) {
    const bool ok = emu_->with_machine([&](Machine& m) {
        if (!m.terminal_key(function, true)) return false;
        m.terminal_key(function, false);
        return true;
    });
    if (!ok) LOG_WARN("input", "this keyboard has no %s key", function);
    SetFocus(hwnd_.load());
}

// ---------------------------------------------------------------------------
// Title and status

std::string Win32Frontend::profile_display_name() const {
    if (!cur_.active) return std::string();
    return (cur_.untitled ? narrow(load_string(IDS_UNTITLED)) : cur_.name) + (cur_.modified ? "*" : "");
}

void Win32Frontend::update_title() {
    std::string t;
    const std::string prof = profile_display_name();
    if (!prof.empty()) t = prof + " - ";
    {
        std::lock_guard<std::mutex> g(text_mu_);
        t += title_.empty() ? "IBM " + model_name_ + " - em34xx" : title_;
        if (!status_note_.empty()) t += " - " + status_note_;
    }
    SetWindowTextW(hwnd_.load(), widen(t).c_str());
}

void Win32Frontend::update_status() {
    const std::vector<TwinaxSessionStatus> sessions = app_->host_link().sessions();
    std::string text[4];
    for (unsigned i = 0; i < 2; ++i) {
        const TwinaxSessionStatus* s = nullptr;
        for (const TwinaxSessionStatus& x : sessions)
            if (!x.printer && x.address == i) s = &x;
        if (!s) {
            text[i] = format("Session %u: %s", i + 1, i == 0 || sessions.empty() ? "not connected" : "-");
            continue;
        }
        std::string state = s->state == "ready" ? "connected" : s->state;
        text[i] = format("Session %u: %s %s", i + 1, state.c_str(), s->peer.c_str());
        if (!s->connected && !s->error.empty()) text[i] += " (" + s->error + ")";
    }
    bool tls = false, tls_up = false;
    for (const TwinaxSessionStatus& x : sessions) {
        tls = tls || x.tls;
        tls_up = tls_up || (x.tls && x.connected);
    }
    if (!tls || !cur_.active) {
        text[2] = narrow(load_string(IDS_TLS_OFF));
    } else if (showing_trust_) {
        // The TCP connection is up but the handshake waits on the prompt.
        text[2] = "TLS: certificate not trusted, waiting";
    } else if (!tls_up) {
        text[2] = "TLS: not connected";
    } else {
        switch (cur_.p.tls_mode) {
        case ProfileTlsMode::System: text[2] = cur_.accepted_once ? "TLS: accepted once" : "TLS: verified"; break;
        case ProfileTlsMode::AcceptAll: text[2] = "TLS: not verified"; break;
        case ProfileTlsMode::Fingerprint:
            text[2] = cur_.accepted_once ? "TLS: accepted once" : "TLS: trusted fingerprint";
            break;
        }
    }
    text[3] = cur_.active ? "Profile: " + profile_display_name() : narrow(load_string(IDS_NO_PROFILE));
    for (int i = 0; i < 4; ++i) {
        if (text[i] == last_status_[i]) continue;
        last_status_[i] = text[i];
        SendMessageW(status_, SB_SETTEXTW, WPARAM(i), reinterpret_cast<LPARAM>(widen(text[i]).c_str()));
    }
    const bool connected = !sessions.empty();
    SendMessageW(toolbar_, TB_ENABLEBUTTON, ID_FILE_DISCONNECT, MAKELPARAM(connected ? TRUE : FALSE, 0));
    SendMessageW(toolbar_, TB_ENABLEBUTTON, ID_FILE_SAVE, MAKELPARAM(cur_.active ? TRUE : FALSE, 0));
}

void Win32Frontend::update_ui_state() {
    update_title();
    for (auto& s : last_status_) s.clear();
    update_status();
}

// ---------------------------------------------------------------------------
// Profiles and connections

void Win32Frontend::connect_profile(const Profile& p) {
    HWND h = hwnd_.load();
    if (p.model != model_name_) {
        error_box(h, "This profile is for the IBM " + p.model + "; the emulator runs an IBM " + model_name_ +
                         ". Change the model in Settings and start em34xxw again.");
        return;
    }
    AppConfig cfg;
    std::string error, message;
    if (!profile_connect_config(p, app_->host_link().base(), settings().tls_min_version, cfg, error)) {
        error_box(h, "Cannot connect: " + error);
        return;
    }
    trust_->cancel_all();
    cur_.accepted_once = false;
    if (!app_->host_link().connect(cfg, message, error)) {
        error_box(h, error);
        return;
    }
    LOG_INFO("tn5250", "%s", trim(message).c_str());
    update_ui_state();
}

void Win32Frontend::disconnect() {
    trust_->cancel_all();
    std::string error;
    app_->host_link().disconnect(error);   // "not connected" is fine
    update_ui_state();
}

void Win32Frontend::open_profile_file(const std::string& path) {
    Profile p;
    std::string error;
    if (!load_profile(path, p, error)) {
        error_box(hwnd_.load(), error);
        return;
    }
    if (!confirm_discard()) return;
    cur_ = CurrentProfile();
    cur_.active = true;
    cur_.p = p;
    const std::filesystem::path fp = std::filesystem::u8path(path);
#if defined(__cpp_char8_t)
    const auto stem = fp.stem().u8string();
    cur_.name = std::string(stem.begin(), stem.end());
#else
    cur_.name = fp.stem().u8string();
#endif
    cur_.path = path;
    connect_profile(cur_.p);
}

// Save (or Save As) the current profile. False when cancelled or failed.
bool Win32Frontend::save_current(bool save_as) {
    HWND h = hwnd_.load();
    if (!cur_.active) return false;
    if (cur_.untitled || save_as) {
        std::string name = cur_.untitled ? dirs().unique_profile_name(cur_.p.sessions[0].host) : cur_.name;
        if (!name_dialog(h, L"Save Profile As", L"&Profile name:", name)) return false;
        if (dirs().profile_exists(name) && (cur_.untitled || name != cur_.name) &&
            message_box(h, "A profile named \"" + name + "\" exists. Replace it?", MB_YESNO | MB_ICONQUESTION) !=
                IDYES)
            return false;
        std::string error;
        if (!save_profile(dirs().profile_path(name), cur_.p, error)) {
            error_box(h, error);
            return false;
        }
        cur_.name = name;
        cur_.path = dirs().profile_path(name);
        cur_.untitled = false;
    } else {
        std::string error;
        if (!save_profile(cur_.path, cur_.p, error)) {
            error_box(h, error);
            return false;
        }
    }
    cur_.modified = false;
    update_ui_state();
    return true;
}

// Before the current profile is replaced or the program ends: offer to
// save unsaved changes. False: the user cancelled.
bool Win32Frontend::confirm_discard() {
    if (!cur_.active || !cur_.modified) return true;
    const std::string name = cur_.untitled ? narrow(load_string(IDS_UNTITLED)) : cur_.name;
    const int r = message_box(hwnd_.load(), string_with(IDS_SAVE_CHANGES, name), MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) return false;
    if (r == IDYES) return save_current(false);
    return true;
}

void Win32Frontend::handle_trust_requests() {
    if (showing_trust_) return;   // the loop below picks up later requests
    showing_trust_ = true;
    release_keys();
    while (auto p = trust_->take()) {
        const std::string name = cur_.active ? (cur_.untitled ? narrow(load_string(IDS_UNTITLED)) : cur_.name)
                                             : std::string("(none)");
        const tn5250::TlsTrustDecision d = trust_dialog(hwnd_.load(), p->req, name);
        if (d == tn5250::TlsTrustDecision::AcceptPermanently && cur_.active) {
            profile_trust_certificate(cur_.p, p->req.station, p->req.peer.sha256);
            std::string error;
            if (!cur_.untitled && save_profile(cur_.path, cur_.p, error)) {
                cur_.modified = false;
            } else {
                cur_.modified = true;
                if (!error.empty()) error_box(hwnd_.load(), error);
            }
        } else if (d == tn5250::TlsTrustDecision::AcceptOnce) {
            cur_.accepted_once = true;
        }
        trust_->answer(p, d);
        // Rejected: keep the session closed instead of reconnecting into the
        // same question.
        if (d == tn5250::TlsTrustDecision::Reject) PostMessageW(hwnd_.load(), WM_APP_HOLD, p->req.station, 0);
        update_ui_state();
    }
    showing_trust_ = false;
}

void Win32Frontend::edit_keymap() {
    HWND h = hwnd_.load();
    release_keys();
    KeymapFile file = pcomm_keymap_file();
    std::string text, error;
    std::error_code ec;
    if (std::filesystem::exists(std::filesystem::u8path(dirs().keymap_file()), ec)) {
        if (!read_text_file(dirs().keymap_file(), text, error) ||
            !parse_keymap_file(text, dirs().keymap_file(), file, error)) {
            error_box(h, error + "\n\nThe keymap editor starts from the default (PCOMM) keymap.");
            file = pcomm_keymap_file();
        }
    }
    if (!keymap_dialog(h, file)) return;
    if (!write_text_file(dirs().keymap_file(),
                         keymap_file_to_toml(file, "em34xx host keymap (written by em34xxw; docs/keyboard.md).\n"
                                                   "Applies to every profile."),
                         error)) {
        error_box(h, error);
        return;
    }
    launch_.keymap_path = dirs().keymap_file();
    const std::string spec = runtime_path(launch_.keymap_path);
    if (!emu_->with_machine([&](Machine& m) { return m.set_keymap(spec, error); }))
        error_box(h, "The keymap was saved but could not be applied: " + error);
}

void Win32Frontend::edit_settings() {
    HWND h = hwnd_.load();
    release_keys();
    AppSettings s = settings();
    bool restart = false;
    if (!settings_dialog(h, dirs(), s, restart)) return;
    const bool crt_changed = s.crt != settings().crt;
    const bool speed_changed = s.speed_percent != settings().speed_percent;
    s.window = settings().window;
    s.zoom = settings().zoom;
    settings() = s;
    std::string error;
    if (!save_settings(dirs().settings_file(), settings(), error)) error_box(h, error);
    if (speed_changed) app_->set_speed_percent(s.speed_percent);
    if (crt_changed) {
        crt_params_.enabled = s.crt;
        CrtParams p = crt_params_;
        app_->set_crt(p);
        crt_.invalidate();
        dirty_ = true;
        schedule_render();
        InvalidateRect(h, nullptr, FALSE);
    }
    if (restart) MessageBoxW(h, load_string(IDS_RESTART_NEEDED).c_str(), load_string(IDS_APP_TITLE).c_str(),
                             MB_OK | MB_ICONINFORMATION);
}

void Win32Frontend::command(int id) {
    HWND h = hwnd_.load();
    switch (id) {
    case ID_FILE_PROFILES: {
        release_keys();
        const std::string name = profile_manager_dialog(h, dirs(), settings());
        if (!name.empty()) open_profile_file(dirs().profile_path(name));
        break;
    }
    case ID_FILE_CONNECT: {
        release_keys();
        Profile p;
        p.model = model_name_;
        if (!profile_editor_dialog(h, p, ProfileEditMode::Connect, settings(), "Connect")) break;
        if (!confirm_discard()) break;
        cur_ = CurrentProfile();
        cur_.active = true;
        cur_.p = p;
        cur_.untitled = true;
        cur_.modified = true;   // never saved
        connect_profile(cur_.p);
        break;
    }
    case ID_FILE_EDIT_PROFILE: {
        if (!cur_.active) {
            command(ID_FILE_CONNECT);
            break;
        }
        release_keys();
        Profile p = cur_.p;
        const std::string title = "Profile: " + (cur_.untitled ? narrow(load_string(IDS_UNTITLED)) : cur_.name);
        if (!profile_editor_dialog(h, p, ProfileEditMode::Connect, settings(), title)) break;
        if (p != cur_.p) {
            cur_.p = p;
            cur_.modified = true;
        }
        connect_profile(cur_.p);
        break;
    }
    case ID_FILE_SAVE: save_current(false); break;
    case ID_FILE_SAVE_AS: save_current(true); break;
    case ID_FILE_DISCONNECT: disconnect(); break;
    case ID_FILE_EXIT: PostMessageW(h, WM_CLOSE, 0, 0); break;
    case ID_EDIT_KEYMAP: edit_keymap(); break;
    case ID_EDIT_SETTINGS: edit_settings(); break;
    case ID_VIEW_ZOOM_100:
    case ID_VIEW_ZOOM_150:
    case ID_VIEW_ZOOM_200: {
        settings().zoom = id == ID_VIEW_ZOOM_100 ? 1.0 : id == ID_VIEW_ZOOM_150 ? 1.5 : 2.0;
        size_for_zoom(true);
        std::string error;
        save_placement();
        save_settings(dirs().settings_file(), settings(), error);
        break;
    }
    case ID_TERM_ATTN: terminal_key("attn"); break;
    case ID_TERM_SYSRQ: terminal_key("sysrq"); break;
    case ID_TERM_RESET: terminal_key("reset"); break;
    case ID_TERM_POWER_CYCLE: release_keys(); app_->power_on(); break;
    case ID_HELP_ABOUT: release_keys(); about_dialog(h, dirs()); break;
    default: break;
    }
    SetFocus(h);
}

// ---------------------------------------------------------------------------
// Messages

LRESULT Win32Frontend::handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        if (toolbar_) layout();
        return 0;
    case WM_ERASEBKGND: {
        // The bars' background (the flat toolbar is transparent and asks
        // its parent); the picture area paints itself.
        HDC dc = reinterpret_cast<HDC>(wp);
        RECT c, d = display_rect();
        GetClientRect(hwnd, &c);
        const int saved = SaveDC(dc);
        ExcludeClipRect(dc, d.left, d.top, d.right, d.bottom);
        FillRect(dc, &c, GetSysColorBrush(COLOR_BTNFACE));
        RestoreDC(dc, saved);
        return 1;
    }
    case WM_PAINT:
        paint();
        return 0;
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wp);
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        build_toolbar_images();
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
        return 0;
    }
    case WM_APP_DISPLAY:
        display_pending_.store(false);
        dirty_ = true;
        schedule_render();
        return 0;
    case WM_TIMER:
        if (wp == kTimerRaster) {
            KillTimer(hwnd, kTimerRaster);
            raster_timer_ = false;
            schedule_render();
        } else if (wp == kTimerStatus) {
            if (emu_->quit_requested() || quit_.load()) {
                PostMessageW(hwnd, WM_APP_QUIT, 0, 0);
                return 0;
            }
            update_status();
        }
        return 0;
    case WM_APP_TITLE:
        update_title();
        return 0;
    case WM_APP_TRUST:
        handle_trust_requests();
        return 0;
    case WM_APP_HOLD: {
        std::string error;
        app_->host_link().disconnect_station(unsigned(wp), error);
        update_status();
        return 0;
    }
    case WM_APP_QUIT:
        cur_.modified = false;   // asked to end: no questions
        DestroyWindow(hwnd);
        return 0;
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        // Alt+F4 still closes the window; every other key (Alt and F10
        // included) goes to the terminal, so the menu opens by mouse only.
        if (msg == WM_SYSKEYDOWN && wp == VK_F4 && GetKeyState(VK_MENU) < 0) {
            release_keys();
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        key(msg, wp, lp);
        return 0;
    case WM_CHAR:
    case WM_SYSCHAR:
    case WM_DEADCHAR:
    case WM_SYSDEADCHAR:
        return 0;
    case WM_KILLFOCUS:
        release_keys();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) release_keys();
        else SetFocus(hwnd);
        return 0;
    case WM_COMMAND:
        command(LOWORD(wp));
        return 0;
    case WM_NOTIFY: {
        NMHDR* n = reinterpret_cast<NMHDR*>(lp);
        if (n->code == TTN_GETDISPINFOW) {
            NMTTDISPINFOW* di = reinterpret_cast<NMTTDISPINFOW*>(lp);
            for (const ToolButton& b : kToolbar)
                if (b.command && UINT_PTR(b.command) == n->idFrom) {
                    di->hinst = launch_.instance;
                    di->lpszText = MAKEINTRESOURCEW(b.text);
                }
            return 0;
        }
        break;
    }
    case WM_INITMENUPOPUP: {
        HMENU m = reinterpret_cast<HMENU>(wp);
        const UINT active = cur_.active ? MF_ENABLED : MF_GRAYED;
        EnableMenuItem(m, ID_FILE_SAVE, MF_BYCOMMAND | active);
        EnableMenuItem(m, ID_FILE_SAVE_AS, MF_BYCOMMAND | active);
        EnableMenuItem(m, ID_FILE_EDIT_PROFILE, MF_BYCOMMAND | active);
        const bool connected = app_->host_link().controller_kind() == "tn5250";
        EnableMenuItem(m, ID_FILE_DISCONNECT, MF_BYCOMMAND | (connected ? MF_ENABLED : MF_GRAYED));
        const double z = settings().zoom;
        CheckMenuRadioItem(m, ID_VIEW_ZOOM_100, ID_VIEW_ZOOM_200,
                           z >= 2.0 ? ID_VIEW_ZOOM_200 : z >= 1.5 ? ID_VIEW_ZOOM_150 : ID_VIEW_ZOOM_100, MF_BYCOMMAND);
        return 0;
    }
    case WM_CLOSE:
        release_keys();
        if (!confirm_discard()) return 0;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: {
        save_placement();
        std::string error;
        save_settings(dirs().settings_file(), settings(), error);
        trust_->close();
        KillTimer(hwnd, kTimerStatus);
        KillTimer(hwnd, kTimerRaster);
        hwnd_.store(nullptr);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool Win32Frontend::run(Application& app, const FrontendOptions& opts, std::string& error) {
    app_ = &app;
    emu_ = &app.emulator();
    model_name_ = app.model().info().name;
    crt_params_ = opts.crt;
    emu_->with_machine([&](Machine& m) { render_presented(m, fb_); });
    base_w_ = std::max(1, fb_.presented_width());
    base_h_ = std::max(1, fb_.presented_height());

    if (!create_window(error)) {
        error_box(nullptr, error);
        return false;
    }
    HWND h = hwnd_.load();
    trust_->attach(h);
    {
        auto q = trust_;
        app.host_link().set_tls_trust_handler([q](const tn5250::TlsTrustRequest& r) { return q->ask(r); });
    }
    emu_->set_display_callback([this](uint64_t) {
        if (display_pending_.exchange(true)) return;
        HWND w = hwnd_.load();
        if (w) PostMessageW(w, WM_APP_DISPLAY, 0, 0);
    });
    struct Cleanup {
        Application& app;
        Emulator& emu;
        ~Cleanup() {
            emu.set_display_callback(nullptr);
            app.host_link().set_tls_trust_handler(nullptr);
        }
    } cleanup{app, *emu_};

    restore_placement();
    layout();
    update_ui_state();
    ShowWindow(h, settings().window.valid && settings().window.maximized ? SW_SHOWMAXIMIZED : launch_.show);
    UpdateWindow(h);
    SetTimer(h, kTimerStatus, 500, nullptr);
    if (!launch_.profile_path.empty()) open_profile_file(launch_.profile_path);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        HWND main = hwnd_.load();
        if (main && accel_ && TranslateAcceleratorW(main, accel_, &msg)) continue;
        // Keys for the main window go straight to it: no WM_CHAR, and no
        // menu activation by Alt / F10.
        if (msg.hwnd == main && msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST) {
            DispatchMessageW(&msg);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (images_) ImageList_Destroy(images_);
    if (font_) DeleteObject(font_);
    images_ = nullptr;
    font_ = nullptr;
    return true;
}

} // namespace

std::unique_ptr<Frontend> make_win32_frontend(Win32Launch& launch) {
    return std::make_unique<Win32Frontend>(launch);
}

// ---------------------------------------------------------------------------
// Launch

bool win32_prepare(Win32Launch& launch) {
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES | ICC_LINK_CLASS;
    InitCommonControlsEx(&icc);

    // The user folder: %APPDATA%\em34xx, or EM34XX_USER_DIR (portable use, tests).
    std::string root;
    wchar_t env[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(L"EM34XX_USER_DIR", env, DWORD(sizeof env / sizeof env[0]));
    if (n > 0 && n < sizeof env / sizeof env[0]) {
        root = narrow(env);
    } else {
        const std::string appdata = appdata_folder();
        if (appdata.empty()) {
            error_box(nullptr, "Cannot find the Application Data folder (CSIDL_APPDATA).");
            launch.exit_code = 2;
            return false;
        }
        root = user_dirs_root(appdata);
    }
    launch.dirs = std::make_unique<UserDirs>(root);
    std::string error;
    if (!launch.dirs->ensure(error)) {
        error_box(nullptr, error);
        launch.exit_code = 2;
        return false;
    }
    if (!load_settings(launch.dirs->settings_file(), launch.settings, error)) {
        error_box(nullptr, error + "\n\nDefault settings are used.");
        launch.settings = AppSettings();
    }

    // Command line: an optional .em34xx profile to open and connect.
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        const std::string a = narrow(argv[i]);
        if (launch.profile_path.empty() && !a.empty() && a[0] != '-') launch.profile_path = a;
        else {
            error_box(nullptr, "Unknown command-line argument: " + a +
                                   "\n\nUsage: em34xxw [PROFILE.em34xx]");
        }
    }
    if (argv) LocalFree(argv);

    // The model: from the settings when it can run, else the first that can.
    const auto models = runnable_models();
    if (models.empty()) {
        error_box(nullptr, "No terminal model of this build can run yet.");
        launch.exit_code = 2;
        return false;
    }
    const Model* model = models.front();
    for (const Model* m : models)
        if (m->info().name == launch.settings.model) model = m;
    launch.settings.model = model->info().name;

    // First start (or ROMs gone): the ROM manager.
    if (!model_roms_ready(*model, launch.dirs->roms()) &&
        !win32::rom_manager_dialog(nullptr, *launch.dirs, model)) {
        error_box(nullptr, "em34xxw cannot start the IBM " + model->info().name +
                               " without its ROM images. Import them when you start em34xxw again.");
        launch.exit_code = 1;
        return false;
    }

    // The global keymap, when there is one and it loads.
    launch.keymap_path.clear();
    std::error_code ec;
    if (std::filesystem::exists(std::filesystem::u8path(launch.dirs->keymap_file()), ec)) {
        std::string text;
        KeymapFile file;
        if (read_text_file(launch.dirs->keymap_file(), text, error) &&
            parse_keymap_file(text, launch.dirs->keymap_file(), file, error))
            launch.keymap_path = launch.dirs->keymap_file();
        else
            error_box(nullptr, error + "\n\nThe default (PCOMM) keymap is used.");
    }

    launch.args = {"em34xxw",
                   "--model",
                   model->info().name,
                   "--rom-dir",
                   runtime_path(launch.dirs->roms()),
                   "--display",
                   display_subtype_name(launch.settings.display),
                   "--speed",
                   std::to_string(launch.settings.speed_percent),
                   "--keymap",
                   launch.keymap_path.empty() ? std::string("pcomm") : runtime_path(launch.keymap_path),
                   "--nvram",
                   runtime_path(launch.dirs->nvram_file(model->info().name)),
                   "--log",
                   "info",
                   "--log-file",
                   runtime_path(launch.dirs->root() + "\\em34xxw.log")};
    if (!launch.settings.crt) launch.args.push_back("--no-crt");
    return true;
}

int win32_run(Application& app, Win32Launch& launch) {
    std::vector<char*> argv;
    for (std::string& a : launch.args) argv.push_back(&a[0]);
    argv.push_back(nullptr);
    if (!app.start(int(launch.args.size()), argv.data())) {
        if (!app.last_error().empty()) error_box(nullptr, app.last_error());
        return app.exit_code();
    }
    return app.run();
}

} // namespace em34xx
