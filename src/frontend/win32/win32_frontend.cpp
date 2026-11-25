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
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
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
constexpr UINT WM_APP_STARTUP = WM_APP + 15;   // the window is up: offer the profiles
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
        if (closed_ || draining_ || !hwnd_) return tn5250::TlsTrustDecision::Reject;
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

    bool empty() {
        std::lock_guard<std::mutex> g(mu_);
        return queue_.empty();
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

    // While sessions are torn down (TrustDrain): the UI thread joins their
    // threads, so everything still waiting is rejected and ask() rejects at
    // once; a session that reaches its certificate check meanwhile must not
    // wait for a prompt the UI thread cannot show. Nests.
    void begin_drain() {
        std::lock_guard<std::mutex> g(mu_);
        ++draining_;
        for (auto& p : queue_) p->done = true;
        queue_.clear();
        cv_.notify_all();
    }
    void end_drain() {
        std::lock_guard<std::mutex> g(mu_);
        if (draining_ > 0) --draining_;
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
    int draining_ = 0;
};

// Around a teardown of sessions (disconnect, machine rebuild).
class TrustDrain {
public:
    explicit TrustDrain(TrustQueue& q) : q_(q) { q_.begin_drain(); }
    ~TrustDrain() { q_.end_drain(); }
    TrustDrain(const TrustDrain&) = delete;
    TrustDrain& operator=(const TrustDrain&) = delete;

private:
    TrustQueue& q_;
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
    // Per session: it runs on a certificate that is not valid (expired, not
    // yet valid, revoked), let through by the user for that connection.
    bool overridden[Profile::kSessions] = {};
};

// Toolbar glyphs in the IDB_TOOLBAR* strips (tools/gen_win32_assets.py).
enum ToolImage {
    kImgProfiles = 0,
    kImgNew = 1,
    kImgSave = 2,
    kImgDisconnect = 3,
    kImgSettings = 4,
    kImgKeymap = 5,
    kImgReconnect = 6,
    kImgReboot = 7,
    kToolImages = 8,
};
// Toolbar: glyph, command, string.
struct ToolButton {
    int image;   // -1: separator; -2: text only
    int command;
    UINT text;
};
const ToolButton kToolbar[] = {
    {kImgNew, ID_FILE_NEW, IDS_TB_CONNECT},
    {kImgProfiles, ID_FILE_PROFILES, IDS_TB_PROFILES},
    {kImgSave, ID_FILE_SAVE, IDS_TB_SAVE},
    {-1, 0, 0},
    {kImgDisconnect, ID_FILE_DISCONNECT, IDS_TB_DISCONNECT},   // a toggle: Reconnect when off line
    {kImgReboot, ID_TERM_POWER_CYCLE, IDS_TB_REBOOT},
    {-1, 0, 0},
    {kImgSettings, ID_EDIT_SETTINGS, IDS_TB_SETTINGS},
    {kImgKeymap, ID_EDIT_KEYMAP, IDS_TB_KEYMAP},
    {-1, 0, 0},
    {-2, ID_TERM_ATTN, IDS_TB_ATTN},
    {-2, ID_TERM_SYSRQ, IDS_TB_SYSRQ},
    {-2, ID_TERM_RESET, IDS_TB_RESET},
    {-2, ID_TERM_SETUP, IDS_TB_SETUP},
};

// Status bar glyphs in the IDB_STATUS* strips: an orb per session and its
// TLS lock.
enum StatusImage {
    kOrbGreen = 0,     // connected
    kOrbGrey = 1,      // not configured
    kOrbRed = 2,       // disconnected or failed
    kOrbOrange = 3,    // connecting (or waiting on the certificate prompt)
    kLockClosed = 4,   // TLS
    kLockOpen = 5,     // plain telnet
    kLockClosedDim = 6,   // TLS without certificate verification
    kLockOpenDim = 7,
    kStatusImages = 8,
};

struct SessionIndicator {
    int orb = kOrbGrey;
    int lock = kLockOpenDim;
    std::string tip;
    bool operator==(const SessionIndicator& o) const { return orb == o.orb && lock == o.lock && tip == o.tip; }
    bool operator!=(const SessionIndicator& o) const { return !(*this == o); }
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
    void draw_status_icons(const DRAWITEMSTRUCT& di);
    RECT session_icon_rect(unsigned session) const;
    SessionIndicator session_indicator(unsigned session, const std::vector<TwinaxSessionStatus>& all) const;
    std::string tls_state_text(const TwinaxSessionStatus* s) const;

    // Apply the profile's terminal settings (display subtype, scan lines).
    // `rebuilt`: the change was visible to the firmware or the host, so the
    // machine was rebuilt and is powering on (its sessions closed).
    bool apply_terminal_settings(const Profile& p, bool& rebuilt);
    // The model a profile names, when it can run now: a model of this
    // build with its ROM images (the user is told which are missing and may
    // import them). Null, after a message, otherwise.
    const Model* profile_model(const Profile& p, const std::string& what);
    enum class ModelSwitch { Same, Switched, Failed };
    // Restart the emulator as the profile's model when it is another one.
    ModelSwitch switch_model(const Profile& p);
    // Apply the profile's terminal settings, then connect: after a power
    // cycle (as switching the terminal on) or, with power_cycle false, by
    // plugging the controller in with the terminal running.
    void connect_profile(const Profile& p, bool power_cycle = true);
    bool connected() const;
    void new_connection();
    void edit_connection();
    // `confirm_foreign`: the path came from outside (the command line, a
    // file association): ask before connecting when the file is not in the
    // user's profiles folder.
    void open_profile_file(const std::string& path, bool confirm_foreign = false);
    bool save_current(bool save_as);
    bool confirm_discard();
    void edit_keymap();
    void edit_settings();
    void handle_trust_requests();
    void disconnect();
    void toggle_connection();
    void reboot();
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
    HWND tips_ = nullptr;            // the status bar's session tooltips
    HACCEL accel_ = nullptr;
    HIMAGELIST images_ = nullptr;
    HIMAGELIST status_images_ = nullptr;
    int status_icon_ = 16;           // status glyph size (px)
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
    unsigned trust_station_ = 0;     // the session the open prompt is for
    bool trust_invalid_ = false;     // the open prompt is the invalid-certificate one
    CurrentProfile cur_;
    std::string last_status_text_;
    SessionIndicator indicators_[Profile::kSessions];
    bool toolbar_connected_ = true;  // the Disconnect/Reconnect toggle as shown
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
    return guarded(hwnd, LRESULT(0), [&] { return self->handle(hwnd, msg, wp, lp); });
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

namespace {
HIMAGELIST load_strip(HINSTANCE inst, int id, int size, int count) {
    HBITMAP bmp =
        static_cast<HBITMAP>(LoadImageW(inst, MAKEINTRESOURCEW(id), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
    HIMAGELIST il = ImageList_Create(size, size, ILC_COLOR32, count, 0);
    if (bmp && il) ImageList_Add(il, bmp, nullptr);
    if (bmp) DeleteObject(bmp);
    return il;
}
} // namespace

void Win32Frontend::build_toolbar_images() {
    const int size = dpi_ >= 168 ? 32 : dpi_ >= 120 ? 24 : 16;
    HIMAGELIST il = load_strip(launch_.instance,
                               size == 32   ? IDB_TOOLBAR32
                               : size == 24 ? IDB_TOOLBAR24
                                            : IDB_TOOLBAR16,
                               size, kToolImages);
    SendMessageW(toolbar_, TB_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(il));
    if (images_) ImageList_Destroy(images_);
    images_ = il;
    // The status glyphs follow the text height: 16 px up to 144 dpi.
    status_icon_ = dpi_ >= 192 ? 32 : dpi_ >= 144 ? 24 : 16;
    HIMAGELIST sl = load_strip(launch_.instance,
                               status_icon_ == 32   ? IDB_STATUS32
                               : status_icon_ == 24 ? IDB_STATUS24
                                                    : IDB_STATUS16,
                               status_icon_, kStatusImages);
    if (status_images_) ImageList_Destroy(status_images_);
    status_images_ = sl;
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
    // One tooltip per session indicator (state, host, TLS).
    tips_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, h, nullptr, launch_.instance,
                            nullptr);
    for (unsigned i = 0; i < Profile::kSessions; ++i) {
        TOOLINFOW ti;
        ZeroMemory(&ti, sizeof ti);
        ti.cbSize = sizeof ti;
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = status_;
        ti.uId = i;
        ti.lpszText = const_cast<wchar_t*>(L"");
        SendMessageW(tips_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    }
    SendMessageW(tips_, TTM_SETMAXTIPWIDTH, 0, 600);
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
            // Attn, SysRq, Reset and Setup show their names; the others are
            // pictures with tooltips (TTN_GETDISPINFO).
            t.iString = b.image == -2 ? reinterpret_cast<INT_PTR>(texts[i].c_str()) : INT_PTR(-1);
        }
        buttons.push_back(t);
    }
    SendMessageW(toolbar_, TB_ADDBUTTONS, WPARAM(buttons.size()), reinterpret_cast<LPARAM>(buttons.data()));
    SendMessageW(toolbar_, TB_AUTOSIZE, 0, 0);
}

// Status bar: the profile name on the left; on the right, per session an
// orb (its state) and a lock (TLS), then room for the size grip.
namespace {
int status_gap(UINT dpi) { return scale_px(2, dpi); }
int status_session_gap(UINT dpi) { return scale_px(8, dpi); }
} // namespace

void Win32Frontend::layout() {
    SendMessageW(toolbar_, TB_AUTOSIZE, 0, 0);
    SendMessageW(status_, WM_SIZE, 0, 0);
    RECT sr;
    GetClientRect(status_, &sr);
    const int w = sr.right;
    const int icons = int(Profile::kSessions) * (2 * status_icon_ + status_gap(dpi_)) +
                      int(Profile::kSessions - 1) * status_session_gap(dpi_) + scale_px(8, dpi_) +
                      GetSystemMetrics(SM_CXVSCROLL);
    const int parts[2] = {std::max(1, w - icons), -1};
    SendMessageW(status_, SB_SETPARTS, 2, reinterpret_cast<LPARAM>(parts));
    SendMessageW(status_, SB_SETTEXTW, 1 | SBT_OWNERDRAW | SBT_NOBORDERS, 0);
    for (unsigned i = 0; i < Profile::kSessions; ++i) {
        TOOLINFOW ti;
        ZeroMemory(&ti, sizeof ti);
        ti.cbSize = sizeof ti;
        ti.hwnd = status_;
        ti.uId = i;
        ti.rect = session_icon_rect(i);
        SendMessageW(tips_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&ti));
    }
    last_status_text_.clear();
    dirty_ = true;
    schedule_render();
}

// Where session `session`'s orb and lock are, in status bar coordinates.
RECT Win32Frontend::session_icon_rect(unsigned session) const {
    RECT part{};
    SendMessageW(status_, SB_GETRECT, 1, reinterpret_cast<LPARAM>(&part));
    const int pair = 2 * status_icon_ + status_gap(dpi_);
    const int x0 = part.left + scale_px(4, dpi_) + int(session) * (pair + status_session_gap(dpi_));
    const int y0 = part.top + std::max(0, int(part.bottom - part.top - status_icon_) / 2);
    return RECT{x0, y0, x0 + pair, y0 + status_icon_};
}

void Win32Frontend::draw_status_icons(const DRAWITEMSTRUCT& di) {
    if (!status_images_) return;
    for (unsigned i = 0; i < Profile::kSessions; ++i) {
        const RECT r = session_icon_rect(i);
        ImageList_Draw(status_images_, indicators_[i].orb, di.hDC, r.left, r.top, ILD_TRANSPARENT);
        ImageList_Draw(status_images_, indicators_[i].lock, di.hDC, r.left + status_icon_ + status_gap(dpi_), r.top,
                       ILD_TRANSPARENT);
    }
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
        t += title_.empty() ? "IBM " + app_->variant().id + " - em34xx" : title_;
        if (!status_note_.empty()) t += " - " + status_note_;
    }
    SetWindowTextW(hwnd_.load(), widen(t).c_str());
}

bool Win32Frontend::connected() const { return app_->host_link().bridging(); }

std::string Win32Frontend::tls_state_text(const TwinaxSessionStatus* s) const {
    if (cur_.p.tls_mode == ProfileTlsMode::AcceptAll) return "TLS, certificate not verified";
    if (!s || !s->connected) return "TLS";
    if (s->address < Profile::kSessions && cur_.overridden[s->address])
        return "TLS, certificate NOT VALID (accepted for this connection)";
    switch (cur_.p.tls_mode) {
    case ProfileTlsMode::System: return cur_.accepted_once ? "TLS, certificate accepted once" : "TLS, verified";
    case ProfileTlsMode::AcceptAll: break;
    case ProfileTlsMode::Fingerprint:
        return cur_.accepted_once ? "TLS, certificate accepted once" : "TLS, trusted certificate";
    }
    return "TLS";
}

SessionIndicator Win32Frontend::session_indicator(unsigned i, const std::vector<TwinaxSessionStatus>& all) const {
    SessionIndicator ind;
    const std::string label = format("Session %u", i + 1);
    if (!cur_.active || !cur_.p.sessions[i].used()) {
        ind.orb = kOrbGrey;
        ind.lock = kLockOpenDim;
        ind.tip = label + ": not configured";
        return ind;
    }
    const ProfileSession& ps = cur_.p.sessions[i];
    const TwinaxSessionStatus* s = nullptr;
    for (const TwinaxSessionStatus& x : all)
        if (!x.printer && x.address == i) s = &x;
    const bool tls = s ? s->tls : ps.tls;
    // Encrypted but with any certificate accepted: not the plain closed lock.
    ind.lock = !tls ? kLockOpen : cur_.p.tls_mode == ProfileTlsMode::AcceptAll ? kLockClosedDim : kLockClosed;
    std::string state;
    if (showing_trust_ && trust_station_ == i) {
        ind.orb = kOrbOrange;
        state = trust_invalid_ ? "connecting, the certificate is not valid: waiting for your answer"
                               : "connecting, the certificate is not trusted: waiting for your answer";
    } else if (!s) {
        ind.orb = kOrbRed;
        state = "disconnected";
    } else if (s->connected && s->state == "ready") {
        ind.orb = kOrbGreen;
        state = "connected";
    } else if (s->held || s->state == "closed" || s->state == "failed" || s->state == "disconnected") {
        ind.orb = kOrbRed;
        state = s->held ? "disconnected" : s->state == "failed" ? "failed" : "closed";
        if (!s->error.empty()) state += " (" + s->error + ")";
        if (!s->held) state += ", retrying";
    } else {
        ind.orb = kOrbOrange;
        state = s->state.empty() ? "connecting" : s->state;
    }
    const std::string peer = s && !s->peer.empty() ? s->peer : format("%s:%u", ps.host.c_str(), ps.port);
    ind.tip = label + ": " + peer + "\n" + state + "\n" + (tls ? tls_state_text(s) : "TLS off (plain telnet)");
    return ind;
}

void Win32Frontend::update_status() {
    const std::vector<TwinaxSessionStatus> sessions = app_->host_link().sessions();
    const std::string text = cur_.active ? "Profile: " + profile_display_name() : narrow(load_string(IDS_NO_PROFILE));
    if (text != last_status_text_) {
        last_status_text_ = text;
        SendMessageW(status_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(widen(text).c_str()));
    }
    bool repaint = false;
    for (unsigned i = 0; i < Profile::kSessions; ++i) {
        const SessionIndicator ind = session_indicator(i, sessions);
        if (ind == indicators_[i]) continue;
        if (ind.tip != indicators_[i].tip) {
            const std::wstring tip = widen(ind.tip);
            TOOLINFOW ti;
            ZeroMemory(&ti, sizeof ti);
            ti.cbSize = sizeof ti;
            ti.hwnd = status_;
            ti.uId = i;
            ti.lpszText = const_cast<wchar_t*>(tip.c_str());
            SendMessageW(tips_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
        }
        indicators_[i] = ind;
        repaint = true;
    }
    if (repaint) {
        RECT part{};
        SendMessageW(status_, SB_GETRECT, 1, reinterpret_cast<LPARAM>(&part));
        InvalidateRect(status_, &part, TRUE);
    }
    // Disconnect / Reconnect toggle.
    const bool on_line = connected();
    if (on_line != toolbar_connected_) {
        toolbar_connected_ = on_line;
        SendMessageW(toolbar_, TB_CHANGEBITMAP, ID_FILE_DISCONNECT,
                     MAKELPARAM(on_line ? kImgDisconnect : kImgReconnect, 0));
    }
    SendMessageW(toolbar_, TB_ENABLEBUTTON, ID_FILE_DISCONNECT,
                 MAKELPARAM(on_line || cur_.active ? TRUE : FALSE, 0));
    SendMessageW(toolbar_, TB_ENABLEBUTTON, ID_FILE_SAVE, MAKELPARAM(cur_.active ? TRUE : FALSE, 0));
}

void Win32Frontend::update_ui_state() {
    update_title();
    last_status_text_.clear();
    update_status();
}

// ---------------------------------------------------------------------------
// Profiles and connections

const Model* Win32Frontend::profile_model(const Profile& p, const std::string& what) {
    HWND h = hwnd_.load();
    const Model* model = nullptr;
    for (const Model* m : runnable_models())
        if (m->info().name == p.model) model = m;
    if (!model) {
        error_box(h, what + " is for the IBM " + p.model + ", which this version of em34xx cannot emulate.");
        return nullptr;
    }
    return ensure_model_roms(h, dirs(), *model, what, &model->variant_named(p.variant)) ? model : nullptr;
}

Win32Frontend::ModelSwitch Win32Frontend::switch_model(const Profile& p) {
    // The same unit: the model and its variant (a profile that names none
    // means the model's first).
    Profile want = p;
    want.normalize();
    if (want.model == model_name_ && want.variant == app_->variant().id) return ModelSwitch::Same;
    HWND h = hwnd_.load();
    const Model* model = profile_model(p, "This connection");
    if (!model) return ModelSwitch::Failed;
    // The new terminal as the profile describes it, with its own
    // battery-backed RAM file (its setup); the keyboard stays.
    TerminalOptions options = emu_->with_machine([](Machine& m) { return m.options(); });
    options.variant = want.variant;
    options.phosphor = want.phosphor;
    // A variant with its own firmware keeps its own (Model::storage_name).
    const std::string stored_as = model->storage_name(model->variant_named(want.variant));
    options.nvram_path = runtime_path(dirs().nvram_file(stored_as));
    options.eeprom_path = runtime_path(dirs().eeprom_file(stored_as));
    std::string error;
    bool ok = false;
    {
        TrustDrain drain(*trust_);   // the rebuild closes the sessions
        release_keys();
        ok = app_->set_model(*model, options, error);
    }
    if (!ok) {
        error_box(h, "Cannot start the IBM " + want.variant + ": " + error);
        return ModelSwitch::Failed;
    }
    model_name_ = model->info().name;
    crt_.invalidate();
    dirty_ = true;
    schedule_render();
    InvalidateRect(h, nullptr, FALSE);
    update_ui_state();
    return ModelSwitch::Switched;
}

bool Win32Frontend::apply_terminal_settings(const Profile& p, bool& rebuilt) {
    HWND h = hwnd_.load();
    rebuilt = false;
    if (crt_params_.enabled != p.crt) {
        crt_params_.enabled = p.crt;
        app_->set_crt(crt_params_);
        crt_.invalidate();
    }
    // The tube of a monochrome unit: the picture only, the terminal keeps
    // running. (Another variant is another unit: switch_model.)
    Profile want = p;
    want.normalize();
    if (!want.phosphor.empty() && want.variant == app_->variant().id) {
        std::string error;
        if (!app_->set_phosphor(want.phosphor, error)) {
            error_box(h, std::string("Cannot change the CRT type: ") + error);
            return false;
        }
    }
    dirty_ = true;
    schedule_render();
    InvalidateRect(h, nullptr, FALSE);
    return true;
}

void Win32Frontend::connect_profile(const Profile& p, bool power_cycle) {
    HWND h = hwnd_.load();
    // Another terminal model: the emulator restarts as it. Without its ROM
    // images nothing connects (the sessions would announce the wrong type).
    const ModelSwitch switched = switch_model(p);
    if (switched == ModelSwitch::Failed) {
        update_ui_state();
        return;
    }
    AppConfig cfg;
    std::string error, message;
    if (!profile_connect_config(p, app_->host_link().base(), settings().tls_min_version, cfg, error)) {
        error_box(h, "Cannot connect: " + error);
        return;
    }
    bool rebuilt = false;
    {
        // The running sessions close here, with the trust queue draining:
        // their threads are joined, and one that reaches its certificate
        // check meanwhile is refused instead of waiting for a prompt. The
        // new sessions start after it and may ask again.
        TrustDrain drain(*trust_);
        apply_terminal_settings(p, rebuilt);
        rebuilt = rebuilt || switched == ModelSwitch::Switched;
        release_keys();
        std::string ignored;
        app_->host_link().disconnect(ignored);   // "not connected" is fine
    }
    cur_.accepted_once = false;
    for (bool& o : cur_.overridden) o = false;
    // Like switching the terminal on: the running sessions close, the
    // terminal runs its power-on self test, and the controller brings the
    // sessions up once it answers the polls. A rebuilt machine is already
    // powering on.
    const bool ok = power_cycle && !rebuilt ? app_->connect_after_power_cycle(cfg, message, error)
                                            : app_->host_link().connect(cfg, message, error);
    if (!ok) {
        error_box(h, error);
        update_ui_state();
        return;
    }
    LOG_INFO("tn5250", "%s", trim(message).c_str());
    update_ui_state();
}

void Win32Frontend::new_connection() {
    release_keys();
    Profile p;
    p.model = model_name_;
    p.variant = app_->variant().id;
    p.phosphor = app_->config().terminal.phosphor;
    if (!profile_editor_dialog(hwnd_.load(), p, ProfileEditMode::Connect, settings(), dirs(), "New Connection"))
        return;
    if (!profile_model(p, "This connection")) return;
    if (!confirm_discard()) return;
    cur_ = CurrentProfile();
    cur_.active = true;
    cur_.p = p;
    cur_.untitled = true;
    cur_.modified = true;   // never saved
    connect_profile(cur_.p);
}

// Edit > Connection Settings: the running profile in the editor. Changes
// mark it modified (saved with File > Save, or asked about on exit or when
// another profile is opened) and apply now: the CRT type at once (or after
// a confirmation when the terminal must restart), connection changes by
// reconnecting the sessions with the terminal running.
void Win32Frontend::edit_connection() {
    HWND h = hwnd_.load();
    if (!cur_.active) {
        new_connection();
        return;
    }
    release_keys();
    Profile p = cur_.p;
    const std::string title =
        "Connection Settings: " + (cur_.untitled ? narrow(load_string(IDS_UNTITLED)) : cur_.name);
    if (!profile_editor_dialog(h, p, ProfileEditMode::Apply, settings(), dirs(), title)) return;
    if (p == cur_.p) return;
    Profile old = cur_.p;
    old.normalize();
    p.normalize();
    // Another terminal type (model or variant): it needs its ROM images and
    // a restart; refused or not possible, the terminal stays and the other
    // changes apply. The CRT type of a monochrome unit changes at once.
    if ((p.model != model_name_ || p.variant != app_->variant().id) &&
        (!profile_model(p, "This connection") ||
         message_box(h, string_with(IDS_MODEL_RESET_CONFIRM, p.variant), MB_YESNO | MB_ICONQUESTION) != IDYES)) {
        p.model = old.model;
        p.variant = old.variant;
        p.phosphor = old.phosphor;
        p.normalize();
    }
    Profile same_terminal = old;
    same_terminal.model = p.model;
    same_terminal.variant = p.variant;
    same_terminal.phosphor = p.phosphor;
    same_terminal.crt = p.crt;
    const bool connection_changed = same_terminal != p;
    const bool was_connected = connected();
    const ModelSwitch switched = switch_model(p);
    if (switched == ModelSwitch::Failed) {
        p.model = old.model;
        p.variant = old.variant;
        p.phosphor = old.phosphor;
    }
    bool rebuilt = false;
    if (!apply_terminal_settings(p, rebuilt)) p.phosphor = app_->config().terminal.phosphor;
    rebuilt = rebuilt || switched == ModelSwitch::Switched;
    if (p != old) {
        cur_.p = p;
        cur_.modified = true;
    }
    // A rebuilt terminal lost its sessions; changed sessions reconnect.
    if (was_connected && (rebuilt || connection_changed)) connect_profile(cur_.p, false);
    update_ui_state();
}

void Win32Frontend::disconnect() {
    {
        TrustDrain drain(*trust_);
        std::string error;
        app_->host_link().disconnect(error);   // "not connected" is fine
    }
    update_ui_state();
}

// The Disconnect toolbar button and menu item: off line -> reconnect the
// current profile (the terminal keeps running, as when plugging the cable
// back in).
void Win32Frontend::toggle_connection() {
    if (connected()) disconnect();
    else if (cur_.active) connect_profile(cur_.p, false);
}

// Terminal > Reboot: switch the terminal off and on. Its sessions end, as
// on a real host; the terminal runs its self test and comes back on line
// to new ones (a new sign-on). After a Disconnect, it also dials the host
// again (Application::power_on()).
void Win32Frontend::reboot() {
    release_keys();
    app_->power_on();
}

namespace {

// Text from a profile file, for a message box: no control characters (a
// host name with line breaks could dress the question up).
std::string printable(const std::string& text) {
    std::string out = text;
    for (char& c : out)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) c = '?';
    return out;
}

// What opening a profile from outside the profiles folder would do: each
// session's host and port and how its certificate is checked (as
// profile_connect_config() applies the profile's TLS settings).
std::string foreign_profile_question(const std::string& path, const Profile& p) {
    std::string text = "This profile is not in your profiles folder:\n\n" + printable(path) +
                       "\n\nOpening it connects to:\n\n";
    for (size_t i = 0; i < Profile::kSessions; ++i) {
        const ProfileSession& ps = p.sessions[i];
        if (!ps.used()) continue;
        text += format("Session %u: %s:%u - ", unsigned(i + 1), printable(ps.host).c_str(), unsigned(ps.port));
        const std::string pin = !ps.cert_sha256.empty()                      ? ps.cert_sha256
                                : p.tls_mode == ProfileTlsMode::Fingerprint ? p.tls_cert_sha256
                                                                            : std::string();
        if (!ps.tls) text += "TLS off (plain telnet, not encrypted)";
        else if (p.tls_mode == ProfileTlsMode::AcceptAll) text += "TLS, certificate NOT VERIFIED";
        else if (!pin.empty()) text += "TLS, only the certificate with SHA-256 " + printable(pin);
        else text += "TLS, verified";
        text += "\n";
    }
    text += "\nA profile from someone else can connect you to a system that imitates your sign-on screen, and "
            "it carries its own certificate settings. Connect only when you know where this file comes from "
            "and recognise these systems.\n\nConnect?";
    return text;
}

} // namespace

void Win32Frontend::open_profile_file(const std::string& path, bool confirm_foreign) {
    Profile p;
    std::string error;
    if (!load_profile(path, p, error)) {
        error_box(hwnd_.load(), error);
        return;
    }
    if (confirm_foreign && !file_in_folder(path, dirs().profiles()) &&
        message_box(hwnd_.load(), foreign_profile_question(path, p), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) !=
            IDYES)
        return;
    // A profile for a terminal whose ROM images are missing: the user is
    // told which files and may import them; otherwise nothing changes.
    if (!profile_model(p, "This profile")) return;
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
    // Another dialog or message box is up (the main window is disabled):
    // the prompt waits until it has closed, so it never opens over a modal
    // dialog and never changes the profile under an open editor. The status
    // timer comes back here.
    if (trust_->empty() || !IsWindowEnabled(hwnd_.load())) return;
    showing_trust_ = true;
    release_keys();
    while (auto p = trust_->take()) {
        const std::string name = cur_.active ? (cur_.untitled ? narrow(load_string(IDS_UNTITLED)) : cur_.name)
                                             : std::string("(none)");
        trust_station_ = p->req.station;
        trust_invalid_ = p->req.problem == tn5250::TlsTrustProblem::Invalid;
        update_status();   // that session's orb: waiting for the answer
        // A certificate that is not valid has its own prompt, with no way
        // to remember the answer: nothing below touches the profile for it,
        // and every connection attempt comes back here.
        tn5250::TlsTrustDecision d;
        if (trust_invalid_) {
            d = certificate_invalid_dialog(hwnd_.load(), p->req) ? tn5250::TlsTrustDecision::AcceptOnce
                                                                 : tn5250::TlsTrustDecision::Reject;
            if (p->req.station < Profile::kSessions)
                cur_.overridden[p->req.station] = d == tn5250::TlsTrustDecision::AcceptOnce;
        } else {
            d = trust_dialog(hwnd_.load(), p->req, name);
            // A later connection of this session that needed no override.
            if (p->req.station < Profile::kSessions && d != tn5250::TlsTrustDecision::Reject)
                cur_.overridden[p->req.station] = false;
        }
        if (trust_invalid_) {
            // Nothing to record.
        } else if (d == tn5250::TlsTrustDecision::AcceptPermanently && cur_.active) {
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
    if (!settings_dialog(h, dirs(), s)) return;
    const bool speed_changed = s.speed_percent != settings().speed_percent;
    s.window = settings().window;
    s.zoom = settings().zoom;
    settings() = s;
    if (speed_changed) app_->set_speed_percent(s.speed_percent);
    std::string error;
    if (!save_settings(dirs().settings_file(), settings(), error)) error_box(h, error);
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
    case ID_FILE_NEW: new_connection(); break;
    case ID_EDIT_CONNECTION: edit_connection(); break;
    case ID_FILE_SAVE: save_current(false); break;
    case ID_FILE_SAVE_AS: save_current(true); break;
    case ID_FILE_DISCONNECT: toggle_connection(); break;
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
    case ID_TERM_SETUP: terminal_key("setup"); break;
    case ID_TERM_POWER_CYCLE: reboot(); break;
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
            handle_trust_requests();   // a prompt that waited for another dialog to close
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
    case WM_APP_STARTUP:
        // No profile on the command line: the startup profile (Settings)
        // when one is set and still exists, else the profile manager,
        // modal over the main window. Cancel leaves the terminal idle.
        if (quit_.load()) return 0;
        // The name is a file name in profiles\ and nothing else (it comes
        // from settings.toml): never a path.
        if (std::string why; !launch_.skip_start_profile && settings().startup_open &&
                             !settings().startup_profile.empty() &&
                             UserDirs::valid_profile_name(settings().startup_profile, why) &&
                             dirs().profile_exists(settings().startup_profile)) {
            open_profile_file(dirs().profile_path(settings().startup_profile));
            SetFocus(hwnd);
        } else {
            command(ID_FILE_PROFILES);
        }
        return 0;
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
        if (n->code == TTN_GETDISPINFOW && n->hwndFrom != tips_) {
            NMTTDISPINFOW* di = reinterpret_cast<NMTTDISPINFOW*>(lp);
            for (const ToolButton& b : kToolbar)
                if (b.command && UINT_PTR(b.command) == n->idFrom) {
                    di->hinst = launch_.instance;
                    di->lpszText = MAKEINTRESOURCEW(b.text);
                }
            if (n->idFrom == ID_FILE_DISCONNECT && !connected()) {
                di->hinst = launch_.instance;
                di->lpszText = MAKEINTRESOURCEW(IDS_TB_RECONNECT);
            }
            return 0;
        }
        break;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
        if (di->hwndItem == status_) {
            draw_status_icons(*di);
            return TRUE;
        }
        break;
    }
    case WM_INITMENUPOPUP: {
        HMENU m = reinterpret_cast<HMENU>(wp);
        const UINT active = cur_.active ? MF_ENABLED : MF_GRAYED;
        EnableMenuItem(m, ID_FILE_SAVE, MF_BYCOMMAND | active);
        EnableMenuItem(m, ID_FILE_SAVE_AS, MF_BYCOMMAND | active);
        const bool on_line = connected();
        if (GetMenuState(m, ID_FILE_DISCONNECT, MF_BYCOMMAND) != UINT(-1)) {
            ModifyMenuW(m, ID_FILE_DISCONNECT, MF_BYCOMMAND | MF_STRING, ID_FILE_DISCONNECT,
                        on_line ? L"&Disconnect\tCtrl+Shift+D" : L"&Reconnect\tCtrl+Shift+D");
            EnableMenuItem(m, ID_FILE_DISCONNECT, MF_BYCOMMAND | (on_line || cur_.active ? MF_ENABLED : MF_GRAYED));
        }
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
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        // Log-off or shutdown: the process may end as soon as this returns,
        // without WM_DESTROY or a clean exit. Write what a clean exit
        // writes: the terminal's battery-backed RAM and setup EEPROM, and
        // the window placement.
        if (wp) {
            std::string error;
            app_->flush_persistent_state(error);   // logs a failure itself
            save_placement();
            save_settings(dirs().settings_file(), settings(), error);
        }
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

    install_dialog_centring();
    struct Unhook {
        ~Unhook() { remove_dialog_centring(); }
    } unhook;
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
    if (!launch_.profile_path.empty() && !launch_.skip_start_profile) open_profile_file(launch_.profile_path, true);
    else PostMessageW(h, WM_APP_STARTUP, 0, 0);   // once the window has painted

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
    if (status_images_) ImageList_Destroy(status_images_);
    if (font_) DeleteObject(font_);
    images_ = nullptr;
    status_images_ = nullptr;
    font_ = nullptr;
    return true;
}

} // namespace

std::unique_ptr<Frontend> make_win32_frontend(Win32Launch& launch) {
    return std::make_unique<Win32Frontend>(launch);
}

// ---------------------------------------------------------------------------
// Launch

namespace {

// The log levels: "info", or EM34XX_LOG (the same spec as --log, e.g.
// "info,wsc=debug,tn5250=debug") to diagnose input or a host session.
std::string log_spec() {
    wchar_t env[256];
    const DWORD n = GetEnvironmentVariableW(L"EM34XX_LOG", env, DWORD(sizeof env / sizeof env[0]));
    return n > 0 && n < sizeof env / sizeof env[0] ? narrow(env) : std::string("info");
}

} // namespace

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

    // The terminal starts as the profile that will be opened describes it
    // (the command-line profile, else the startup profile: model, CRT type,
    // scan lines), else as a new profile would: the first model whose ROM
    // images are there, green, with scan lines. Connecting applies a
    // profile's settings in any case.
    if (runnable_models().empty()) {
        error_box(nullptr, "No terminal model of this build can run yet.");
        launch.exit_code = 2;
        return false;
    }
    Profile start;
    std::string start_name;   // "" = no profile asks for a model
    {
        std::string path = launch.profile_path, why, ignored;   // load errors: reported when it is opened
        if (path.empty() && launch.settings.startup_open && !launch.settings.startup_profile.empty() &&
            UserDirs::valid_profile_name(launch.settings.startup_profile, why) &&
            launch.dirs->profile_exists(launch.settings.startup_profile))
            path = launch.dirs->profile_path(launch.settings.startup_profile);
        if (!path.empty()) {
            if (load_profile(path, start, ignored)) start_name = narrow(std::filesystem::u8path(path).stem().wstring());
            else start = Profile();
        }
    }

    // One complete set of ROM images is enough to start; the other models
    // stay selectable (greyed) until theirs are imported. A profile for a
    // model without its images: say which files, offer to import them, and
    // otherwise start without the profile.
    const std::string roms = launch.dirs->roms();
    // What the profile asks for: its variant, else its model.
    const std::string wanted = start.variant.empty() ? start.model : start.variant;
    StartModel choice = choose_start_model(start_name.empty() ? std::string() : wanted, roms);
    bool declined = false;
    if (choice.wanted_unready) {
        declined = !win32::ensure_model_roms(nullptr, *launch.dirs, *choice.wanted_unready,
                                             "The profile \"" + start_name + "\"", choice.wanted_variant);
        choice = choose_start_model(wanted, roms);
        if (declined) {
            launch.skip_start_profile = true;
            start = Profile();
        }
    }
    // First start (or ROMs gone): the ROM manager, until one model can run.
    if (!choice.model && !declined && win32::rom_manager_dialog(nullptr, *launch.dirs, nullptr, true))
        choice = choose_start_model(start_name.empty() ? std::string() : wanted, roms);
    if (!choice.model) {
        error_box(nullptr, "em34xxw needs the ROM images of at least one terminal model to start. Import them "
                           "when you start em34xxw again.");
        launch.exit_code = 1;
        return false;
    }
    const Model* model = choice.model;
    const ModelVariant& variant = choice.variant ? *choice.variant : model->default_variant();
    const std::string stored_as = model->storage_name(variant);

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
                   // The profile's variant when it can run, else the first
                   // variant that can.
                   variant.id,
                   "--rom-dir",
                   runtime_path(launch.dirs->roms()),
                   "--speed",
                   std::to_string(launch.settings.speed_percent),
                   "--keymap",
                   launch.keymap_path.empty() ? std::string("pcomm") : runtime_path(launch.keymap_path),
                   "--nvram",
                   runtime_path(launch.dirs->nvram_file(stored_as)),
                   // The setup EEPROM (monitor alignment values) is kept too.
                   "--eeprom",
                   runtime_path(launch.dirs->eeprom_file(stored_as)),
                   // An end-user build keeps running on an access the model does
                   // not decode (the default, stop, is for the debugger).
                   "--unmapped",
                   "log",
                   "--log",
                   log_spec(),
                   "--log-file",
                   runtime_path(launch.dirs->root() + "\\em34xxw.log")};
    if (!start.crt) launch.args.push_back("--no-crt");
    if (start.model == model->info().name && !start.phosphor.empty() && variant.phosphor(start.phosphor)) {
        launch.args.push_back("--phosphor");
        launch.args.push_back(start.phosphor);
    }
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
