// SPDX-License-Identifier: GPL-3.0-or-later
#import "frontend/cocoa/cocoa_frontend.h"

#import "frontend/cocoa/cocoa_dialogs.h"
#import "frontend/cocoa/cocoa_util.h"

#include "app/application.hpp"
#include "app/host_link.hpp"
#include "config/profile.hpp"
#include "config/settings.hpp"
#include "config/user_dirs.hpp"
#include "em34xx_version.h"
#include "input/host_keymap.hpp"
#include "machine/emulator.hpp"
#include "models/model.hpp"
#include "present/crt_render.hpp"
#include "present/display_profile.hpp"
#include "util/log.hpp"
#include "util/strings.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <set>

using namespace em34xx;
using namespace em34xx::cocoa;
using steady = std::chrono::steady_clock;

@class EMMainController;

namespace {

constexpr auto kMinRasterInterval = std::chrono::milliseconds(15);
constexpr CGFloat kStatusBarHeight = 24;

// --- TLS trust requests: I/O threads wait, the main thread answers ----------

class TrustQueue {
public:
    struct Pending {
        tn5250::TlsTrustRequest req;
        bool done = false;
        tn5250::TlsTrustDecision decision = tn5250::TlsTrustDecision::Reject;
    };

    // `wake` is called (on the asking thread) when a request is queued.
    void attach(std::function<void()> wake) {
        std::lock_guard<std::mutex> g(mu_);
        wake_ = std::move(wake);
    }

    // On a session's I/O thread.
    tn5250::TlsTrustDecision ask(const tn5250::TlsTrustRequest& req) {
        std::unique_lock<std::mutex> lock(mu_);
        if (closed_ || draining_ || !wake_) return tn5250::TlsTrustDecision::Reject;
        auto p = std::make_shared<Pending>();
        p->req = req;
        queue_.push_back(p);
        wake_();
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

    // While sessions are torn down (TrustDrain): the main thread joins their
    // threads, so everything still waiting is rejected and ask() rejects at
    // once. Nests.
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
        wake_ = nullptr;
        for (auto& p : queue_) p->done = true;
        queue_.clear();
        cv_.notify_all();
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<Pending>> queue_;
    std::function<void()> wake_;
    bool closed_ = false;
    int draining_ = 0;
};

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
    std::string name;   // file stem; "" when untitled
    std::string path;
    bool untitled = false;
    bool modified = false;
    bool accepted_once = false;   // a certificate was accepted for this connection only
    // Per session: it runs on a certificate that is not valid, let through
    // by the user for that connection.
    bool overridden[Profile::kSessions] = {};
};

enum class Orb { Green, Grey, Red, Orange };
enum class Lock { Closed, Open, ClosedDim, OpenDim };

struct SessionIndicator {
    Orb orb = Orb::Grey;
    Lock lock = Lock::OpenDim;
    std::string tip;
    bool operator==(const SessionIndicator& o) const { return orb == o.orb && lock == o.lock && tip == o.tip; }
    bool operator!=(const SessionIndicator& o) const { return !(*this == o); }
};

// What the launch prepared for the Application and the window.
struct Launch {
    std::unique_ptr<UserDirs> dirs;
    AppSettings settings;
    std::string keymap_path;    // the global keymap file, or "" for the built-in Mac map
    std::string log_file;       // the process's log
};

// Text from a profile file, for an alert: no control characters (a host
// name with line breaks could dress the question up).
std::string printable(const std::string& text) {
    std::string out = text;
    for (char& c : out)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) c = '?';
    return out;
}

// What opening a profile from outside the profiles folder would do.
std::string foreign_profile_details(const std::string& path, const Profile& p) {
    std::string text = printable(path) + "\n\nOpening it connects to:\n";
    for (size_t i = 0; i < Profile::kSessions; ++i) {
        const ProfileSession& ps = p.sessions[i];
        if (!ps.used()) continue;
        text += format("\nSession %u: %s:%u - ", unsigned(i + 1), printable(ps.host).c_str(), unsigned(ps.port));
        const std::string pin = !ps.cert_sha256.empty()                      ? ps.cert_sha256
                                : p.tls_mode == ProfileTlsMode::Fingerprint ? p.tls_cert_sha256
                                                                            : std::string();
        if (!ps.tls) text += "TLS off (plain telnet, not encrypted)";
        else if (p.tls_mode == ProfileTlsMode::AcceptAll) text += "TLS, certificate NOT VERIFIED";
        else if (!pin.empty()) text += "TLS, only the certificate with SHA-256 " + printable(pin);
        else text += "TLS, verified";
    }
    text += "\n\nA profile from someone else can connect you to a system that imitates your sign-on screen, and "
            "it carries its own certificate settings. Connect only when you know where this file comes from and "
            "recognise these systems.";
    return text;
}

std::string stem_of(const std::string& path) {
    const std::filesystem::path fp = std::filesystem::u8path(path);
#if defined(__cpp_char8_t)
    const auto stem = fp.stem().u8string();
    return std::string(stem.begin(), stem.end());
#else
    return fp.stem().u8string();
#endif
}

} // namespace

// ===========================================================================
// The Frontend the Application talks to (any thread)

namespace {
class CocoaFrontend : public Frontend {
public:
    const char* name() const override { return "cocoa"; }
    // NSApplication owns the run loop; Application::run() is not used.
    bool run(Application&, const FrontendOptions&, std::string& error) override {
        error = "the Cocoa front end is driven by NSApplication";
        return false;
    }
    void request_quit() override;
    void set_title(const std::string& title) override {
        {
            std::lock_guard<std::mutex> g(mu);
            title_ = title;
        }
        changed();
    }
    void set_status(const std::string& status) override {
        {
            std::lock_guard<std::mutex> g(mu);
            status_ = status;
        }
        changed();
    }
    std::string title() {
        std::lock_guard<std::mutex> g(mu);
        return title_;
    }
    std::string status() {
        std::lock_guard<std::mutex> g(mu);
        return status_;
    }
    __weak EMMainController* controller = nil;

private:
    void changed();
    std::mutex mu;
    std::string title_, status_;
};
} // namespace

// ===========================================================================
// Session indicator: an orb (state) and a lock (TLS), with a tooltip

@interface EMSessionIndicator : NSView
@property(nonatomic) Orb orb;
@property(nonatomic) Lock lock;
@end

@implementation EMSessionIndicator
- (NSSize)intrinsicContentSize {
    return NSMakeSize(34, 16);
}
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    NSColor* c = _orb == Orb::Green    ? NSColor.systemGreenColor
                 : _orb == Orb::Red    ? NSColor.systemRedColor
                 : _orb == Orb::Orange ? NSColor.systemOrangeColor
                                       : NSColor.tertiaryLabelColor;
    const NSRect orb = NSMakeRect(2, (self.bounds.size.height - 10) / 2, 10, 10);
    [c setFill];
    [[NSBezierPath bezierPathWithOvalInRect:orb] fill];
    [[c shadowWithLevel:0.3] setStroke];
    NSBezierPath* rim = [NSBezierPath bezierPathWithOvalInRect:NSInsetRect(orb, 0.5, 0.5)];
    rim.lineWidth = 0.5;
    [rim stroke];
    const bool closed = _lock == Lock::Closed || _lock == Lock::ClosedDim;
    const bool dim = _lock == Lock::ClosedDim || _lock == Lock::OpenDim;
    NSImage* img = [NSImage imageWithSystemSymbolName:closed ? @"lock.fill" : @"lock.open.fill"
                             accessibilityDescription:closed ? @"TLS" : @"no TLS"];
    NSImageSymbolConfiguration* cfg = [NSImageSymbolConfiguration configurationWithPointSize:11
                                                                                      weight:NSFontWeightRegular];
    img = [img imageWithSymbolConfiguration:cfg];
    NSImage* tinted = [NSImage imageWithSize:img.size
                                     flipped:NO
                              drawingHandler:^BOOL(NSRect r) {
                                [img drawInRect:r];
                                [(dim ? NSColor.tertiaryLabelColor : NSColor.secondaryLabelColor) set];
                                NSRectFillUsingOperation(r, NSCompositingOperationSourceAtop);
                                return YES;
                              }];
    const NSSize s = tinted.size;
    [tinted drawInRect:NSMakeRect(18, (self.bounds.size.height - s.height) / 2, s.width, s.height)];
}
@end

// ===========================================================================
// The terminal view: picture and keyboard

@interface EMTerminalView : NSView
@property(weak) EMMainController* controller;
@end

// ===========================================================================
// The main window's controller

// The app delegate, told when a terminal window has closed.
@protocol EMTerminalHost
- (void)terminalClosed:(EMMainController*)terminal;
@end

// One terminal window: its own emulated terminal (an Application with its
// machine, emulation thread and TN5250 sessions) and the profile it runs.
// A window controller, so it is in its window's responder chain: menu and
// toolbar commands with no target reach the terminal in front.
@interface EMMainController : NSWindowController <NSWindowDelegate, NSToolbarDelegate, NSMenuItemValidation,
                                                  NSToolbarItemValidation>
- (instancetype)initWithApp:(std::unique_ptr<Application>)app
                     launch:(Launch*)launch
                   frontend:(CocoaFrontend*)fe
                    primary:(bool)primary;
- (void)showTerminal;
// No profile opened yet: a profile opened from elsewhere may use this window.
- (bool)idle;
- (std::string)profilePath;
// Take this profile and connect it (after a power cycle).
- (void)adoptProfile:(const Profile&)p path:(const std::string&)path untitled:(bool)untitled;
- (bool)confirmDiscard;
// Close the sessions, stop the terminal and write its battery-backed RAM.
- (void)shutdownTerminal;
- (void)setKeymapSpec:(const std::string&)spec;
- (void)setSpeed:(unsigned)pct;
- (void)displayChanged;
- (void)titleChanged;
- (void)quitRequested;
- (void)handleTrustRequests;
// From the terminal view.
- (void)keyEvent:(NSEvent*)e down:(bool)down;
- (void)flagsEvent:(NSEvent*)e;
- (void)releaseKeys;
- (void)viewResized;
- (void)drawPicture:(NSRect)dirty inView:(NSView*)view;
@property bool quitting;
@end

@implementation EMTerminalView
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (BOOL)isOpaque {
    return YES;
}
- (BOOL)resignFirstResponder {
    [self.controller releaseKeys];
    return YES;
}
- (void)keyDown:(NSEvent*)e {
    [self.controller keyEvent:e down:true];
}
- (void)keyUp:(NSEvent*)e {
    [self.controller keyEvent:e down:false];
}
- (void)flagsChanged:(NSEvent*)e {
    [self.controller flagsEvent:e];
}
- (void)drawRect:(NSRect)dirty {
    [self.controller drawPicture:dirty inView:self];
}
- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    [self.controller viewResized];
}
- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    [self.controller viewResized];
}
// Clicks focus the terminal; nothing else happens on a click.
- (void)mouseDown:(NSEvent*)e {
    (void)e;
    [self.window makeFirstResponder:self];
}
@end

static NSToolbarItemIdentifier const kTbNew = @"new";
static NSToolbarItemIdentifier const kTbProfiles = @"profiles";
static NSToolbarItemIdentifier const kTbSave = @"save";
static NSToolbarItemIdentifier const kTbConnection = @"connection";
static NSToolbarItemIdentifier const kTbReboot = @"reboot";
static NSToolbarItemIdentifier const kTbSettings = @"settings";
static NSToolbarItemIdentifier const kTbKeymap = @"keymap";
static NSToolbarItemIdentifier const kTbTermKeys = @"termkeys";

@implementation EMMainController {
    std::unique_ptr<Application> _appOwner;
    Application* _app;
    bool _primary;
    Emulator* _emu;
    Launch* _launch;
    CocoaFrontend* _fe;
    std::string _modelName;
    NSWindow* _win;
    EMTerminalView* _view;
    NSTextField* _statusText;
    EMSessionIndicator* _indicators[Profile::kSessions];
    NSToolbarItem* _connectionItem;
    NSTimer* _statusTimer;
    NSTimer* _rasterTimer;

    Framebuffer _fb;
    CrtRenderer _crt;
    CrtParams _crtParams;
    std::vector<uint32_t> _image;
    int _imgW, _imgH;
    NSRect _imgRect;   // where the image is, in view points
    int _baseW, _baseH;
    bool _dirty;
    steady::time_point _lastRaster;
    std::shared_ptr<std::atomic<bool>> _displayPending;

    std::set<uint32_t> _held;
    std::shared_ptr<TrustQueue> _trust;
    bool _showingTrust;
    unsigned _trustStation;
    bool _trustInvalid;
    CurrentProfile _cur;
    std::string _lastStatusText;
    SessionIndicator _ind[Profile::kSessions];
    bool _toolbarOnline;
}

- (instancetype)initWithApp:(std::unique_ptr<Application>)app
                     launch:(Launch*)launch
                   frontend:(CocoaFrontend*)fe
                    primary:(bool)primary {
    if ((self = [super initWithWindow:nil])) {
        _appOwner = std::move(app);
        _app = _appOwner.get();
        _primary = primary;
        _emu = &_app->emulator();
        _launch = launch;
        _fe = fe;
        _modelName = _app->model().info().name;
        _crtParams = _app->frontend_options().crt;
        _imgW = _imgH = 0;
        _imgRect = NSZeroRect;
        _dirty = true;
        _displayPending = std::make_shared<std::atomic<bool>>(false);
        _trust = std::make_shared<TrustQueue>();
        _showingTrust = false;
        _trustStation = 0;
        _trustInvalid = false;
        _toolbarOnline = true;
        _emu->with_machine([&](Machine& m) { render_presented(m, _fb); });
        _baseW = std::max(1, _fb.presented_width());
        _baseH = std::max(1, _fb.presented_height());
        [self buildWindow];
        self.window = _win;
    }
    return self;
}

- (AppSettings&)settings {
    return _launch->settings;
}
- (const UserDirs&)dirs {
    return *_launch->dirs;
}

// --- window ----------------------------------------------------------------------

- (void)buildWindow {
    _win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 800, 600)
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                    NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    _win.releasedWhenClosed = NO;
    _win.delegate = self;
    _win.title = @"em34xx";
    // Several terminals may share one window as tabs (Window > Merge All
    // Windows, or "Prefer tabs" in System Settings).
    _win.tabbingIdentifier = @"em34xx.terminal";
    _win.restorable = NO;   // terminals come back from their profiles, not from saved state
    _win.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    _win.contentMinSize = NSMakeSize(320, 240);
    if (@available(macOS 11.0, *)) _win.toolbarStyle = NSWindowToolbarStyleUnified;

    NSToolbar* tb = [[NSToolbar alloc] initWithIdentifier:@"em34xx.terminal"];
    tb.delegate = self;
    tb.allowsUserCustomization = YES;
    tb.autosavesConfiguration = YES;
    tb.displayMode = NSToolbarDisplayModeIconOnly;
    _win.toolbar = tb;

    NSView* root = [NSView new];
    _view = [EMTerminalView new];
    _view.controller = self;
    _view.translatesAutoresizingMaskIntoConstraints = NO;
    _view.wantsLayer = YES;
    _view.layerContentsRedrawPolicy = NSViewLayerContentsRedrawOnSetNeedsDisplay;

    // The status bar: the profile on the left, the session indicators right.
    NSView* bar = [NSView new];
    bar.translatesAutoresizingMaskIntoConstraints = NO;
    NSBox* line = [NSBox new];
    line.boxType = NSBoxSeparator;
    line.translatesAutoresizingMaskIntoConstraints = NO;
    _statusText = small_label(@"");
    _statusText.textColor = NSColor.secondaryLabelColor;
    _statusText.lineBreakMode = NSLineBreakByTruncatingTail;
    [_statusText setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                          forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSMutableArray<NSView*>* inds = [NSMutableArray array];
    for (size_t i = 0; i < Profile::kSessions; ++i) {
        _indicators[i] = [EMSessionIndicator new];
        _indicators[i].translatesAutoresizingMaskIntoConstraints = NO;
        _indicators[i].toolTip = @"";
        [inds addObject:_indicators[i]];
    }
    NSStackView* indStack = hstack(inds, 10);
    indStack.alignment = NSLayoutAttributeCenterY;
    [bar addSubview:line];
    [bar addSubview:_statusText];
    [bar addSubview:indStack];
    [root addSubview:_view];
    [root addSubview:bar];
    [NSLayoutConstraint activateConstraints:@[
        [_view.topAnchor constraintEqualToAnchor:root.topAnchor],
        [_view.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
        [_view.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
        [_view.bottomAnchor constraintEqualToAnchor:bar.topAnchor],
        [bar.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
        [bar.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
        [bar.bottomAnchor constraintEqualToAnchor:root.bottomAnchor],
        [bar.heightAnchor constraintEqualToConstant:kStatusBarHeight],
        [line.topAnchor constraintEqualToAnchor:bar.topAnchor],
        [line.leadingAnchor constraintEqualToAnchor:bar.leadingAnchor],
        [line.trailingAnchor constraintEqualToAnchor:bar.trailingAnchor],
        [_statusText.leadingAnchor constraintEqualToAnchor:bar.leadingAnchor constant:10],
        [_statusText.centerYAnchor constraintEqualToAnchor:bar.centerYAnchor],
        [_statusText.trailingAnchor constraintLessThanOrEqualToAnchor:indStack.leadingAnchor constant:-10],
        [indStack.trailingAnchor constraintEqualToAnchor:bar.trailingAnchor constant:-12],
        [indStack.centerYAnchor constraintEqualToAnchor:bar.centerYAnchor],
    ]];
    _win.contentView = root;
    _win.initialFirstResponder = _view;
}

// Content size: the presented frame times the zoom (in points: a Retina
// screen shows it with twice the pixels), plus the status bar; shrunk to
// the screen when it does not fit. The top-left corner stays put.
- (void)sizeForZoom {
    if (_win.styleMask & NSWindowStyleMaskFullScreen) return;
    NSScreen* screen = _win.screen ? _win.screen : NSScreen.mainScreen;
    const NSRect visible = screen.visibleFrame;
    double scale = [self settings].zoom;
    NSRect frame = NSZeroRect;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const NSSize content = NSMakeSize(std::round(_baseW * scale), std::round(_baseH * scale) + kStatusBarHeight);
        frame = [_win frameRectForContentRect:NSMakeRect(0, 0, content.width, content.height)];
        const double fit = std::min(visible.size.width / frame.size.width, visible.size.height / frame.size.height);
        if (fit >= 1.0) break;
        scale *= fit * 0.98;
    }
    const NSRect old = _win.frame;
    frame.origin.x = old.origin.x;
    frame.origin.y = NSMaxY(old) - frame.size.height;
    frame.origin.x = std::min(std::max(frame.origin.x, visible.origin.x), NSMaxX(visible) - frame.size.width);
    frame.origin.y = std::min(std::max(frame.origin.y, visible.origin.y), NSMaxY(visible) - frame.size.height);
    [_win setFrame:frame display:YES animate:_win.visible];
}

- (void)showTerminal {
    // The first window: the last frame (NSUserDefaults), or the zoom size
    // centred. Others: the zoom size, cascaded from the window in front.
    if (_primary) {
        if (![_win setFrameUsingName:@"em34xx.terminal"]) {
            [self sizeForZoom];
            [_win center];
        }
        _win.frameAutosaveName = @"em34xx.terminal";
    } else {
        [self sizeForZoom];
        NSWindow* front = NSApp.mainWindow ? NSApp.mainWindow : NSApp.keyWindow;
        if (front && front != _win) {
            const NSRect f = front.frame;
            [_win cascadeTopLeftFromPoint:[_win cascadeTopLeftFromPoint:NSMakePoint(f.origin.x, NSMaxY(f))]];
        } else {
            [_win center];
        }
    }

    {
        std::weak_ptr<std::atomic<bool>> pending = _displayPending;
        __weak EMMainController* weak = self;
        _emu->set_display_callback([pending, weak](uint64_t) {
            auto p = pending.lock();
            if (!p || p->exchange(true)) return;
            dispatch_async(dispatch_get_main_queue(), ^{
              [weak displayChanged];
            });
        });
        _trust->attach([weak] {
            dispatch_async(dispatch_get_main_queue(), ^{
              [weak handleTrustRequests];
            });
        });
        auto q = _trust;
        _app->host_link().set_tls_trust_handler([q](const tn5250::TlsTrustRequest& r) { return q->ask(r); });
    }
    [self updateUi];
    [_win makeKeyAndOrderFront:nil];
    __weak EMMainController* weak = self;
    _statusTimer = [NSTimer timerWithTimeInterval:0.5
                                          repeats:YES
                                            block:^(NSTimer*) {
                                              [weak statusTick];
                                            }];
    [NSRunLoop.mainRunLoop addTimer:_statusTimer forMode:NSRunLoopCommonModes];
}

- (bool)idle {
    return !_cur.active && !_quitting;
}

- (std::string)profilePath {
    return _cur.active && !_cur.untitled ? _cur.path : std::string();
}

- (void)statusTick {
    guarded([&] {
        if (!_app) return;
        if (_emu->quit_requested()) {
            [self quitRequested];
            return;
        }
        [self handleTrustRequests];   // a prompt that waited for another dialog to close
        [self updateStatus];
    });
}

- (void)shutdownTerminal {
    if (!_appOwner) return;
    [_statusTimer invalidate];
    _statusTimer = nil;
    [_rasterTimer invalidate];
    _rasterTimer = nil;
    [self releaseKeys];
    _trust->close();
    _emu->set_display_callback(nullptr);
    _app->host_link().set_tls_trust_handler(nullptr);
    std::string error;
    save_settings([self dirs].settings_file(), [self settings], error);
    // As at a clean exit: the sessions and the emulation thread wind down
    // and the battery-backed RAM is written (Application::finish()).
    guarded([&] {
        _app->request_quit(0);
        _app->finish();
    });
    _fe->controller = nil;
    _appOwner.reset();
    _app = nullptr;
    _emu = nullptr;
}

// The terminal asked to end (or the program is ending without questions):
// this window closes.
- (void)quitRequested {
    if (_quitting) return;
    _quitting = true;
    _cur.modified = false;
    [_win performClose:nil];
}

// --- picture ------------------------------------------------------------------------

- (void)displayChanged {
    _displayPending->store(false);
    _dirty = true;
    [self scheduleRender];
}

- (void)viewResized {
    _dirty = true;
    [self scheduleRender];
}

- (void)scheduleRender {
    if (!_dirty || !_win || !_app) return;
    const auto now = steady::now();
    const auto due = _lastRaster + kMinRasterInterval;
    if (now >= due) {
        guarded([&] { [self renderNow]; });
        return;
    }
    if (_rasterTimer) return;
    const double s = std::chrono::duration<double>(due - now).count() + 0.001;
    __weak EMMainController* weak = self;
    _rasterTimer = [NSTimer timerWithTimeInterval:s
                                          repeats:NO
                                            block:^(NSTimer*) {
                                              EMMainController* c = weak;
                                              if (!c) return;
                                              c->_rasterTimer = nil;
                                              [c scheduleRender];
                                            }];
    [NSRunLoop.mainRunLoop addTimer:_rasterTimer forMode:NSRunLoopCommonModes];
}

- (void)renderNow {
    _dirty = false;
    _lastRaster = steady::now();
    _emu->with_machine([&](Machine& m) { render_presented(m, _fb); });
    const NSRect b = _view.bounds;
    const NSSize px = [_view convertSizeToBacking:b.size];
    const CGFloat scale = b.size.width > 0 ? px.width / b.size.width : 1.0;
    const int dw = std::max(1, int(px.width)), dh = std::max(1, int(px.height));
    int out_w = 1, out_h = 1;
    crt_fit(_fb, dw, dh, out_w, out_h);
    const bool resized = out_w != _imgW || out_h != _imgH;
    if (resized) {
        _imgW = out_w;
        _imgH = out_h;
        _crt.invalidate();
    }
    // Centred, on whole pixels; the view is not flipped (y up).
    const int x0 = (dw - out_w) / 2, y0 = (dh - out_h) / 2;
    const NSRect where = NSMakeRect(x0 / scale, y0 / scale, out_w / scale, out_h / scale);
    const bool moved = !NSEqualRects(where, _imgRect);
    _imgRect = where;
    const CrtRenderer::Rows rows = _crt.render(_fb, out_w, out_h, _crtParams, _image);
    if (resized || moved) {
        _view.needsDisplay = YES;
    } else if (!rows.empty()) {
        // Image rows row0..row1 counted from the top.
        const CGFloat top = NSMaxY(where) - rows.row0 / scale, bottom = NSMaxY(where) - rows.row1 / scale;
        [_view setNeedsDisplayInRect:NSMakeRect(where.origin.x, bottom - 1, where.size.width, top - bottom + 2)];
    }
}

- (void)drawPicture:(NSRect)dirty inView:(NSView*)view {
    (void)view;
    CGContextRef ctx = NSGraphicsContext.currentContext.CGContext;
    CGContextSetRGBFillColor(ctx, 0, 0, 0, 1);
    CGContextFillRect(ctx, dirty);
    if (_imgW <= 0 || _imgH <= 0 || _image.size() < size_t(_imgW) * size_t(_imgH)) return;
    // The renderer's 0xAARRGGBB words, read in place (drawing is
    // synchronous; the buffer only changes on this thread).
    CGDataProviderRef data =
        CGDataProviderCreateWithData(nullptr, _image.data(), size_t(_imgW) * size_t(_imgH) * 4, nullptr);
    CGColorSpaceRef rgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef img = CGImageCreate(size_t(_imgW), size_t(_imgH), 8, 32, size_t(_imgW) * 4, rgb,
                                   kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, data, nullptr, false,
                                   kCGRenderingIntentDefault);
    CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
    CGContextDrawImage(ctx, NSRectToCGRect(_imgRect), img);
    CGImageRelease(img);
    CGColorSpaceRelease(rgb);
    CGDataProviderRelease(data);
}

// --- keyboard ------------------------------------------------------------------------

- (void)sendKey:(uint32_t)hid down:(bool)down {
    if (down) {
        if (!_held.insert(hid).second) return;
    } else if (!_held.erase(hid)) {
        return;   // never sent down (a key pressed before the focus came)
    }
    HostKeyEvent ev;
    ev.hid_usage = hid;
    ev.pressed = down;
    ev.name = host_key_name(hid);
    _emu->with_machine([&](Machine& m) { m.key_event(ev); });
}

- (void)keyEvent:(NSEvent*)e down:(bool)down {
    if (!_emu) return;
    guarded([&] {
        // Command shortcuts belong to the Mac (menus); the terminal does its
        // own typematic, so the Mac's auto-repeats are dropped.
        if (down && ((e.modifierFlags & NSEventModifierFlagCommand) || e.ARepeat)) return;
        const uint32_t hid = hid_from_keycode(e.keyCode);
        if (hid) [self sendKey:hid down:down];
    });
}

- (void)flagsEvent:(NSEvent*)e {
    if (!_emu) return;
    guarded([&] {
        const uint32_t hid = hid_from_keycode(e.keyCode);
        if (!hid) return;
        if (hid == 0xE3 || hid == 0xE7) {
            // Command is not sent; macOS delivers no key-up for keys
            // released while it is held, so everything is released now.
            if (modifier_down(e.keyCode, e.modifierFlags)) [self releaseKeys];
            return;
        }
        if (hid == 0x39) {
            // Caps Lock reports each toggle once: one press of the key.
            [self sendKey:hid down:true];
            [self sendKey:hid down:false];
            return;
        }
        [self sendKey:hid down:modifier_down(e.keyCode, e.modifierFlags)];
    });
}

- (void)releaseKeys {
    if (_held.empty() || !_emu) return;
    const std::set<uint32_t> held = _held;
    _held.clear();
    _emu->with_machine([&](Machine& m) {
        for (uint32_t hid : held) {
            HostKeyEvent ev;
            ev.hid_usage = hid;
            ev.pressed = false;
            ev.name = host_key_name(hid);
            m.key_event(ev);
        }
    });
}

- (void)terminalKey:(const char*)function {
    const bool ok = _emu->with_machine([&](Machine& m) {
        if (!m.terminal_key(function, true)) return false;
        m.terminal_key(function, false);
        return true;
    });
    if (!ok) LOG_WARN("input", "this keyboard has no %s key", function);
    [_win makeFirstResponder:_view];
}

- (void)windowDidResignKey:(NSNotification*)n {
    (void)n;
    guarded([&] { [self releaseKeys]; });
}

- (void)windowDidBecomeKey:(NSNotification*)n {
    (void)n;
    if (!_win.attachedSheet) [_win makeFirstResponder:_view];
}

// --- title and status ---------------------------------------------------------------

- (std::string)profileName {
    return _cur.untitled ? std::string("Untitled Profile") : _cur.name;
}

- (void)titleChanged {
    if (!_app) return;
    const std::string override_title = _fe->title(), note = _fe->status();
    _win.title = _cur.active ? ns([self profileName]) : @"em34xx";
    std::string sub = override_title.empty() ? "IBM " + _app->variant().id : override_title;
    if (!note.empty()) sub += " — " + note;
    if (@available(macOS 11.0, *)) _win.subtitle = ns(sub);
    _win.documentEdited = _cur.active && _cur.modified;
    _win.representedURL =
        _cur.active && !_cur.untitled && !_cur.path.empty() ? [NSURL fileURLWithPath:ns(_cur.path)] : nil;
}

- (bool)connected {
    return _app->host_link().controller_kind() == "tn5250";
}

- (std::string)tlsStateText:(const TwinaxSessionStatus*)s {
    if (_cur.p.tls_mode == ProfileTlsMode::AcceptAll) return "TLS, certificate not verified";
    if (!s || !s->connected) return "TLS";
    if (s->address < Profile::kSessions && _cur.overridden[s->address])
        return "TLS, certificate NOT VALID (accepted for this connection)";
    switch (_cur.p.tls_mode) {
    case ProfileTlsMode::System: return _cur.accepted_once ? "TLS, certificate accepted once" : "TLS, verified";
    case ProfileTlsMode::AcceptAll: break;
    case ProfileTlsMode::Fingerprint:
        return _cur.accepted_once ? "TLS, certificate accepted once" : "TLS, trusted certificate";
    }
    return "TLS";
}

- (SessionIndicator)indicator:(unsigned)i sessions:(const std::vector<TwinaxSessionStatus>&)all {
    SessionIndicator ind;
    const std::string label = format("Session %u", i + 1);
    if (!_cur.active || !_cur.p.sessions[i].used()) {
        ind.tip = label + ": not configured";
        return ind;
    }
    const ProfileSession& ps = _cur.p.sessions[i];
    const TwinaxSessionStatus* s = nullptr;
    for (const TwinaxSessionStatus& x : all)
        if (!x.printer && x.address == i) s = &x;
    const bool tls = s ? s->tls : ps.tls;
    ind.lock = !tls ? Lock::Open : _cur.p.tls_mode == ProfileTlsMode::AcceptAll ? Lock::ClosedDim : Lock::Closed;
    std::string state;
    if (_showingTrust && _trustStation == i) {
        ind.orb = Orb::Orange;
        state = _trustInvalid ? "connecting, the certificate is not valid: waiting for your answer"
                              : "connecting, the certificate is not trusted: waiting for your answer";
    } else if (!s) {
        ind.orb = Orb::Red;
        state = "disconnected";
    } else if (s->connected && s->state == "ready") {
        ind.orb = Orb::Green;
        state = "connected";
    } else if (s->held || s->state == "closed" || s->state == "failed" || s->state == "disconnected") {
        ind.orb = Orb::Red;
        state = s->held ? "disconnected" : s->state == "failed" ? "failed" : "closed";
        if (!s->error.empty()) state += " (" + s->error + ")";
        if (!s->held) state += ", retrying";
    } else {
        ind.orb = Orb::Orange;
        state = s->state.empty() ? "connecting" : s->state;
    }
    const std::string peer = s && !s->peer.empty() ? s->peer : format("%s:%u", ps.host.c_str(), unsigned(ps.port));
    ind.tip = label + ": " + peer + "\n" + state + "\n" + (tls ? [self tlsStateText:s] : "TLS off (plain telnet)");
    return ind;
}

- (void)updateStatus {
    const std::vector<TwinaxSessionStatus> sessions = _app->host_link().sessions();
    const std::string text =
        _cur.active ? "Profile: " + [self profileName] + (_cur.modified ? " — Edited" : "") : "No profile";
    if (text != _lastStatusText) {
        _lastStatusText = text;
        _statusText.stringValue = ns(text);
    }
    for (unsigned i = 0; i < Profile::kSessions; ++i) {
        const SessionIndicator ind = [self indicator:i sessions:sessions];
        if (ind == _ind[i]) continue;
        _ind[i] = ind;
        _indicators[i].orb = ind.orb;
        _indicators[i].lock = ind.lock;
        _indicators[i].toolTip = ns(ind.tip);
        _indicators[i].needsDisplay = YES;
    }
    // The Disconnect / Reconnect toggle.
    const bool online = [self connected];
    if (online != _toolbarOnline) {
        _toolbarOnline = online;
        [self updateConnectionItem];
    }
    [_win.toolbar validateVisibleItems];
}

- (void)updateUi {
    [self titleChanged];
    _lastStatusText.clear();
    [self updateStatus];
}

// --- toolbar --------------------------------------------------------------------------

- (NSToolbarItem*)item:(NSToolbarItemIdentifier)ident
                 label:(NSString*)label
                symbol:(NSString*)symbol
                action:(SEL)action
                   tip:(NSString*)tip {
    NSToolbarItem* it = [[NSToolbarItem alloc] initWithItemIdentifier:ident];
    it.label = label;
    it.paletteLabel = label;
    it.toolTip = tip;
    it.image = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:label];
    it.target = nil;   // the responder chain: this terminal, or the app delegate
    it.action = action;
    if (@available(macOS 11.0, *)) it.bordered = YES;
    return it;
}

- (void)updateConnectionItem {
    if (!_connectionItem) return;
    const bool online = _toolbarOnline;
    _connectionItem.label = online ? @"Disconnect" : @"Reconnect";
    _connectionItem.toolTip = online ? @"Disconnect" : @"Reconnect";
    _connectionItem.image = [NSImage imageWithSystemSymbolName:online ? @"bolt.slash" : @"bolt"
                                      accessibilityDescription:_connectionItem.label];
}

- (NSToolbarItem*)toolbar:(NSToolbar*)toolbar
        itemForItemIdentifier:(NSToolbarItemIdentifier)ident
    willBeInsertedIntoToolbar:(BOOL)flag {
    (void)toolbar;
    (void)flag;
    if ([ident isEqualToString:kTbNew])
        return [self item:ident label:@"New Connection" symbol:@"plus" action:@selector(newConnection:)
                      tip:@"New Connection"];
    if ([ident isEqualToString:kTbProfiles])
        return [self item:ident label:@"Profiles" symbol:@"list.bullet" action:@selector(connectToProfile:)
                      tip:@"Connect to Profile"];
    if ([ident isEqualToString:kTbSave])
        return [self item:ident label:@"Save" symbol:@"square.and.arrow.down" action:@selector(saveProfile:)
                      tip:@"Save Profile"];
    if ([ident isEqualToString:kTbConnection]) {
        _connectionItem = [self item:ident label:@"Disconnect" symbol:@"bolt.slash"
                              action:@selector(toggleConnection:) tip:@"Disconnect"];
        _connectionItem.paletteLabel = @"Disconnect / Reconnect";
        [self updateConnectionItem];
        return _connectionItem;
    }
    if ([ident isEqualToString:kTbReboot])
        return [self item:ident label:@"Reboot" symbol:@"power" action:@selector(reboot:)
                      tip:@"Reboot (Power Cycle)"];
    if ([ident isEqualToString:kTbSettings])
        return [self item:ident label:@"Settings" symbol:@"gearshape" action:@selector(showSettings:)
                      tip:@"Settings"];
    if ([ident isEqualToString:kTbKeymap])
        return [self item:ident label:@"Keymap" symbol:@"keyboard" action:@selector(editKeymap:) tip:@"Keymap"];
    if ([ident isEqualToString:kTbTermKeys]) {
        // The terminal's own keys as a group of text buttons.
        NSArray<NSString*>* titles = @[ @"Attn", @"SysRq", @"Reset", @"Setup" ];
        NSArray<NSString*>* tips = @[
            @"Press the terminal's Attn key", @"Press the terminal's SysRq key", @"Press the terminal's Reset key",
            @"Press the terminal's Setup key"
        ];
        NSToolbarItemGroup* g =
            [NSToolbarItemGroup groupWithItemIdentifier:ident
                                                 titles:titles
                                          selectionMode:NSToolbarItemGroupSelectionModeMomentary
                                                 labels:titles
                                                 target:nil
                                                 action:@selector(terminalKeyGroup:)];
        g.label = @"Terminal Keys";
        g.paletteLabel = @"Terminal Keys";
        for (NSUInteger i = 0; i < g.subitems.count; ++i) g.subitems[i].toolTip = tips[i];
        return g;
    }
    return nil;
}

- (NSArray<NSToolbarItemIdentifier>*)toolbarDefaultItemIdentifiers:(NSToolbar*)toolbar {
    (void)toolbar;
    return @[
        kTbNew, kTbProfiles, kTbSave, NSToolbarSpaceItemIdentifier, kTbConnection, kTbReboot,
        NSToolbarFlexibleSpaceItemIdentifier, kTbTermKeys, NSToolbarSpaceItemIdentifier, kTbKeymap
    ];
}

- (NSArray<NSToolbarItemIdentifier>*)toolbarAllowedItemIdentifiers:(NSToolbar*)toolbar {
    (void)toolbar;
    return @[
        kTbNew, kTbProfiles, kTbSave, kTbConnection, kTbReboot, kTbSettings, kTbKeymap, kTbTermKeys,
        NSToolbarSpaceItemIdentifier, NSToolbarFlexibleSpaceItemIdentifier
    ];
}

- (BOOL)enabled:(SEL)action {
    if (!_app) return NO;
    if (action == @selector(saveProfile:) || action == @selector(saveProfileAs:)) return _cur.active;
    if (action == @selector(toggleConnection:)) return [self connected] || _cur.active;
    return YES;
}

- (BOOL)validateToolbarItem:(NSToolbarItem*)item {
    return [self enabled:item.action];
}

- (BOOL)validateMenuItem:(NSMenuItem*)item {
    if (item.action == @selector(toggleConnection:)) item.title = [self connected] ? @"Disconnect" : @"Reconnect";
    if (item.action == @selector(zoomLevel:)) {
        const double z = [self settings].zoom;
        const NSInteger cur = z >= 2.0 ? 200 : z >= 1.5 ? 150 : 100;
        item.state = item.tag == cur ? NSControlStateValueOn : NSControlStateValueOff;
    }
    if (item.action == @selector(zoomIn:)) return [self settings].zoom < 2.0;
    if (item.action == @selector(zoomOut:)) return [self settings].zoom > 1.0;
    return [self enabled:item.action];
}

// --- profiles and connections --------------------------------------------------------

// The model a profile names, when it can run now: a model of this build
// with its ROM images (the user is told which are missing and may import
// them). Null, after an alert, otherwise.
- (const Model*)profileModel:(const Profile&)p what:(const std::string&)what {
    const Model* model = nullptr;
    for (const Model* m : runnable_models())
        if (m->info().name == p.model) model = m;
    if (!model) {
        error_alert(_win, what + " is for the IBM " + p.model + ".", "This version of em34xx cannot emulate it.");
        return nullptr;
    }
    return ensure_model_roms(_win, [self dirs], *model, what) ? model : nullptr;
}

// Restart the emulator as the profile's model when it is another one:
// 0 = the same model, 1 = switched, -1 = not possible (after an alert).
- (int)switchModel:(const Profile&)p {
    // The same unit: the model and its variant (a profile that names none
    // means the model's first).
    Profile want = p;
    want.normalize();
    if (want.model == _modelName && want.variant == _app->variant().id) return 0;
    const Model* model = [self profileModel:p what:"This connection"];
    if (!model) return -1;
    // The new terminal as the profile describes it, with its own
    // battery-backed RAM file (its setup); the keyboard stays.
    TerminalOptions options = _emu->with_machine([](Machine& m) { return m.options(); });
    options.variant = want.variant;
    options.phosphor = want.phosphor;
    options.nvram_path = [self dirs].nvram_file(model->info().name);
    options.eeprom_path = [self dirs].eeprom_file(model->info().name);
    std::string error;
    bool ok = false;
    {
        TrustDrain drain(*_trust);   // the rebuild closes the sessions
        [self releaseKeys];
        ok = _app->set_model(*model, options, error);
    }
    if (!ok) {
        error_alert(_win, "Cannot start the IBM " + want.variant, error);
        return -1;
    }
    _modelName = model->info().name;
    _crt.invalidate();
    _dirty = true;
    [self scheduleRender];
    _view.needsDisplay = YES;
    [self updateUi];
    return 1;
}

- (bool)applyTerminalSettings:(const Profile&)p rebuilt:(bool&)rebuilt {
    rebuilt = false;
    if (_crtParams.enabled != p.crt) {
        _crtParams.enabled = p.crt;
        _app->set_crt(_crtParams);
        _crt.invalidate();
    }
    // The tube of a monochrome unit: the picture only, the terminal keeps
    // running. (Another variant is another unit: switchModel.)
    Profile want = p;
    want.normalize();
    if (!want.phosphor.empty() && want.variant == _app->variant().id) {
        std::string error;
        if (!_app->set_phosphor(want.phosphor, error)) {
            error_alert(_win, "Cannot change the CRT type", error);
            return false;
        }
    }
    _dirty = true;
    [self scheduleRender];
    _view.needsDisplay = YES;
    return true;
}

// Apply the profile's terminal settings, then connect: after a power cycle
// (as switching the terminal on) or, with powerCycle false, by plugging the
// controller in with the terminal running.
- (void)connectProfile:(const Profile&)p powerCycle:(bool)powerCycle {
    // Another terminal model: the emulator restarts as it. Without its ROM
    // images nothing connects (the sessions would announce the wrong type).
    const int switched = [self switchModel:p];
    if (switched < 0) {
        [self updateUi];
        return;
    }
    AppConfig cfg;
    std::string error, message;
    if (!profile_connect_config(p, _app->host_link().base(), [self settings].tls_min_version, cfg, error)) {
        error_alert(_win, "Cannot connect", error);
        return;
    }
    bool rebuilt = false;
    {
        // The running sessions close with the trust queue draining (their
        // threads are joined); the new sessions may ask again.
        TrustDrain drain(*_trust);
        [self applyTerminalSettings:p rebuilt:rebuilt];
        rebuilt = rebuilt || switched > 0;
        [self releaseKeys];
        std::string ignored;
        _app->host_link().disconnect(ignored);   // "not connected" is fine
    }
    _cur.accepted_once = false;
    for (bool& o : _cur.overridden) o = false;
    const bool ok = powerCycle && !rebuilt ? _app->connect_after_power_cycle(cfg, message, error)
                                           : _app->host_link().connect(cfg, message, error);
    if (!ok) {
        error_alert(_win, "Cannot connect", error);
        [self updateUi];
        return;
    }
    LOG_INFO("tn5250", "%s", trim(message).c_str());
    [self updateUi];
}

// File > Connection Settings: the running profile in the editor. Changes
// mark it edited and apply now: the CRT type at once (or after a
// confirmation when the terminal must restart), connection changes by
// reconnecting the sessions with the terminal running.
- (void)editConnection:(id)sender {
    guarded([&] {
        if (!_cur.active) {
            [NSApp sendAction:@selector(newConnection:) to:nil from:sender];
            return;
        }
        [self releaseKeys];
        Profile p = _cur.p;
        if (!profile_editor_dialog(_win, p, ProfileEditMode::Apply, [self settings], [self dirs],
                                   "Connection Settings: " + [self profileName]))
            return;
        if (p == _cur.p) return;
        Profile old = _cur.p;
        old.normalize();
        p.normalize();
        // Another terminal type (model or variant): it needs its ROM images
        // and a restart; refused or not possible, the terminal stays and
        // the other changes apply. The CRT type of a monochrome unit
        // changes at once.
        if ((p.model != _modelName || p.variant != _app->variant().id) &&
            (![self profileModel:p what:"This connection"] ||
             !ask(_win, "Change the terminal type to IBM " + p.variant + "?",
                  "This restarts the emulator as that terminal and reconnects the sessions.", "Restart",
                  "Keep the IBM " + _app->variant().id, NSAlertStyleWarning))) {
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
        const bool was_connected = [self connected];
        const int switched = [self switchModel:p];
        if (switched < 0) {
            p.model = old.model;
            p.variant = old.variant;
            p.phosphor = old.phosphor;
        }
        bool rebuilt = false;
        if (![self applyTerminalSettings:p rebuilt:rebuilt]) p.phosphor = _app->config().terminal.phosphor;
        rebuilt = rebuilt || switched > 0;
        if (p != old) {
            _cur.p = p;
            _cur.modified = true;
        }
        // A rebuilt terminal lost its sessions; changed sessions reconnect.
        if (was_connected && (rebuilt || connection_changed)) [self connectProfile:_cur.p powerCycle:false];
        [self updateUi];
    });
}

- (void)disconnect {
    {
        TrustDrain drain(*_trust);
        std::string error;
        _app->host_link().disconnect(error);   // "not connected" is fine
    }
    [self updateUi];
}

- (void)toggleConnection:(id)sender {
    (void)sender;
    guarded([&] {
        if ([self connected]) [self disconnect];
        else if (_cur.active) [self connectProfile:_cur.p powerCycle:false];
        [_win makeFirstResponder:_view];
    });
}

- (void)reboot:(id)sender {
    (void)sender;
    guarded([&] {
        [self releaseKeys];
        _app->power_on();
        [_win makeFirstResponder:_view];
    });
}

- (void)adoptProfile:(const Profile&)p path:(const std::string&)path untitled:(bool)untitled {
    LOG_DEBUG("ui", "window %p takes the profile %s", (__bridge void*)self, path.c_str());
    guarded([&] {
        _cur = CurrentProfile();
        _cur.active = true;
        _cur.p = p;
        _cur.untitled = untitled;
        _cur.modified = untitled;   // never saved
        if (!untitled) {
            _cur.name = stem_of(path);
            _cur.path = path;
        }
        [self connectProfile:_cur.p powerCycle:true];
        [_win makeFirstResponder:_view];
    });
}

- (void)setKeymapSpec:(const std::string&)spec {
    if (!_emu) return;
    std::string error;
    if (!_emu->with_machine([&](Machine& m) { return m.set_keymap(spec, error); }))
        error_alert(_win, "The keymap was saved but could not be applied", error);
}

- (void)setSpeed:(unsigned)pct {
    if (_app) _app->set_speed_percent(pct);
}

// Save (or Save As) the current profile. False when cancelled or failed.
- (bool)saveCurrent:(bool)saveAs {
    if (!_cur.active) return false;
    const UserDirs& dirs = [self dirs];
    if (_cur.untitled || saveAs) {
        std::string name = _cur.untitled ? dirs.unique_profile_name(_cur.p.sessions[0].host) : _cur.name;
        if (!name_dialog(_win, "Save Profile As", "Name of the profile:", "Save", name)) return false;
        if (dirs.profile_exists(name) && (_cur.untitled || name != _cur.name) &&
            !ask(_win, "A profile named \"" + name + "\" already exists. Replace it?",
                 "Replacing it overwrites its settings.", "Replace", "Cancel", NSAlertStyleWarning, true))
            return false;
        std::string error;
        if (!save_profile(dirs.profile_path(name), _cur.p, error)) {
            error_alert(_win, "Cannot save the profile", error);
            return false;
        }
        _cur.name = name;
        _cur.path = dirs.profile_path(name);
        _cur.untitled = false;
        refresh_settings_window();
    } else {
        std::string error;
        if (!save_profile(_cur.path, _cur.p, error)) {
            error_alert(_win, "Cannot save the profile", error);
            return false;
        }
    }
    _cur.modified = false;
    [self updateUi];
    return true;
}

- (void)saveProfile:(id)sender {
    (void)sender;
    guarded([&] {
        [self releaseKeys];
        [self saveCurrent:false];
    });
}

- (void)saveProfileAs:(id)sender {
    (void)sender;
    guarded([&] {
        [self releaseKeys];
        [self saveCurrent:true];
    });
}

// Before the current profile is replaced or the program ends: offer to
// save unsaved changes. False: the user cancelled.
- (bool)confirmDiscard {
    if (!_cur.active || !_cur.modified) return true;
    [self releaseKeys];
    NSAlert* a = [NSAlert new];
    a.messageText = ns("Do you want to save the changes to the profile \"" + [self profileName] + "\"?");
    a.informativeText = @"Your changes will be lost if you don't save them.";
    [a addButtonWithTitle:@"Save"];
    [a addButtonWithTitle:@"Cancel"];
    NSButton* dont = [a addButtonWithTitle:@"Don't Save"];
    dont.keyEquivalent = @"d";
    dont.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    const NSModalResponse r = run_alert(a, _win);
    if (r == NSAlertSecondButtonReturn) return false;
    if (r == NSAlertFirstButtonReturn) return [self saveCurrent:false];
    return true;
}

- (void)handleTrustRequests {
    guarded([&] {
        if (_showingTrust || _quitting) return;   // the loop below picks up later requests
        // Another sheet, alert or modal window is up: the prompt waits until
        // it has closed, so it never changes the profile under an open
        // editor. The status timer comes back here.
        if (_trust->empty() || modal_ui_active(_win)) return;
        _showingTrust = true;
        [self releaseKeys];
        while (auto p = _trust->take()) {
            const std::string name = _cur.active ? [self profileName] : std::string("(none)");
            _trustStation = p->req.station;
            _trustInvalid = p->req.problem == tn5250::TlsTrustProblem::Invalid;
            [self updateStatus];   // that session's orb: waiting for the answer
            // A certificate that is not valid has its own prompt, with no way
            // to remember the answer.
            tn5250::TlsTrustDecision d;
            if (_trustInvalid) {
                d = certificate_invalid_dialog(_win, p->req) ? tn5250::TlsTrustDecision::AcceptOnce
                                                                : tn5250::TlsTrustDecision::Reject;
                if (p->req.station < Profile::kSessions)
                    _cur.overridden[p->req.station] = d == tn5250::TlsTrustDecision::AcceptOnce;
            } else {
                d = trust_dialog(_win, p->req, name);
                if (p->req.station < Profile::kSessions && d != tn5250::TlsTrustDecision::Reject)
                    _cur.overridden[p->req.station] = false;
            }
            if (_trustInvalid) {
                // Nothing to record.
            } else if (d == tn5250::TlsTrustDecision::AcceptPermanently && _cur.active) {
                profile_trust_certificate(_cur.p, p->req.station, p->req.peer.sha256);
                std::string error;
                if (!_cur.untitled && save_profile(_cur.path, _cur.p, error)) {
                    _cur.modified = false;
                } else {
                    _cur.modified = true;
                    if (!error.empty()) error_alert(_win, "Cannot save the profile", error);
                }
            } else if (d == tn5250::TlsTrustDecision::AcceptOnce) {
                _cur.accepted_once = true;
            }
            _trust->answer(p, d);
            // Rejected: keep the session closed instead of reconnecting into
            // the same question.
            if (d == tn5250::TlsTrustDecision::Reject) {
                const unsigned station = p->req.station;
                __weak EMMainController* weak = self;
                dispatch_async(dispatch_get_main_queue(), ^{
                  EMMainController* c = weak;
                  if (!c || c->_quitting) return;
                  guarded([&] {
                      std::string error;
                      c->_app->host_link().disconnect_station(station, error);
                      [c updateStatus];
                  });
                });
            }
            [self updateUi];
        }
        _showingTrust = false;
        [_win makeFirstResponder:_view];
    });
}

// --- keymap, settings, view, terminal ---------------------------------------------

- (void)setZoom:(double)z {
    [self settings].zoom = snap_zoom(z);
    [self sizeForZoom];
    std::string error;
    save_settings([self dirs].settings_file(), [self settings], error);
}

- (void)zoomLevel:(NSMenuItem*)sender {
    guarded([&] { [self setZoom:double(sender.tag) / 100.0]; });
}

- (void)zoomIn:(id)sender {
    (void)sender;
    guarded([&] {
        for (double z : kZoomLevels)
            if (z > [self settings].zoom + 0.01) {
                [self setZoom:z];
                return;
            }
    });
}

- (void)zoomOut:(id)sender {
    (void)sender;
    guarded([&] {
        double best = kZoomLevels[0];
        for (double z : kZoomLevels)
            if (z < [self settings].zoom - 0.01) best = z;
        [self setZoom:best];
    });
}

- (void)terminalKeyGroup:(NSToolbarItemGroup*)sender {
    static const char* const kKeys[] = {"attn", "sysrq", "reset", "setup"};
    const NSInteger i = sender.selectedIndex;
    if (i >= 0 && i < 4) guarded([&] { [self terminalKey:kKeys[i]]; });
}

- (void)attn:(id)sender {
    (void)sender;
    guarded([&] { [self terminalKey:"attn"]; });
}
- (void)sysRq:(id)sender {
    (void)sender;
    guarded([&] { [self terminalKey:"sysrq"]; });
}
- (void)terminalReset:(id)sender {
    (void)sender;
    guarded([&] { [self terminalKey:"reset"]; });
}
- (void)setup:(id)sender {
    (void)sender;
    guarded([&] { [self terminalKey:"setup"]; });
}

// --- window events ------------------------------------------------------------------

// Closing a window ends its terminal only (after the question about an
// edited profile); the program keeps running, as Mac apps do.
- (BOOL)windowShouldClose:(NSWindow*)w {
    (void)w;
    if (!_quitting && ![self confirmDiscard]) return NO;
    _quitting = true;
    [self shutdownTerminal];
    return YES;
}

- (void)windowWillClose:(NSNotification*)n {
    (void)n;
    [self shutdownTerminal];
    [(id<EMTerminalHost>)NSApp.delegate terminalClosed:self];
}

- (void)windowDidChangeScreen:(NSNotification*)n {
    (void)n;
    [self viewResized];
}

@end

void CocoaFrontend::request_quit() {
    __weak EMMainController* c = controller;
    dispatch_async(dispatch_get_main_queue(), ^{
      [c quitRequested];
    });
}

void CocoaFrontend::changed() {
    __weak EMMainController* c = controller;
    dispatch_async(dispatch_get_main_queue(), ^{
      [c titleChanged];
    });
}

// ===========================================================================
// Application delegate: menus, launch, Finder's open requests, termination

@interface EMAppDelegate : NSObject <NSApplicationDelegate, NSMenuItemValidation, NSToolbarItemValidation,
                                     EMTerminalHost>
@end

@implementation EMAppDelegate {
    std::unique_ptr<Launch> _launch;
    NSMutableArray<EMMainController*>* _terminals;
    std::vector<std::string> _argv;
    std::vector<std::string> _pendingProfiles;   // opened from Finder before the launch finished
    const Model* _model;         // the model a new terminal without a profile starts as
    bool _skipStartupProfile;    // its model's ROM images were not provided at launch
    bool _launched;
    bool _logOpen;
    bool _terminating;
    int _exitCode;
}

- (instancetype)initWithArgs:(std::vector<std::string>)args {
    if ((self = [super init])) {
        _argv = std::move(args);
        _terminals = [NSMutableArray array];
        _model = nullptr;
        _skipStartupProfile = false;
        _launched = false;
        _logOpen = false;
        _terminating = false;
        _exitCode = 0;
    }
    return self;
}

- (int)exitCode {
    return _exitCode;
}

- (void)quitWithoutQuestions {
    for (EMMainController* t in _terminals) t.quitting = true;
    [NSApp terminate:nil];
}

- (NSMenu*)menu:(NSString*)title in:(NSMenu*)bar {
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
    [bar addItem:item];
    NSMenu* m = [[NSMenu alloc] initWithTitle:title];
    item.submenu = m;
    return m;
}

- (NSMenuItem*)add:(NSString*)title action:(SEL)action key:(NSString*)key to:(NSMenu*)menu {
    NSMenuItem* it = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key ? key : @""];
    [menu addItem:it];
    return it;
}

- (void)buildMenus {
    NSMenu* bar = [NSMenu new];
    NSMenu* app = [self menu:@"em34xx" in:bar];
    [self add:@"About em34xx" action:@selector(about:) key:nil to:app].target = self;
    [app addItem:NSMenuItem.separatorItem];
    NSString* settings = @"Preferences…";
    if (@available(macOS 13.0, *)) settings = @"Settings…";
    [self add:settings action:@selector(showSettings:) key:@"," to:app];
    [self add:@"Keymap…" action:@selector(editKeymap:) key:nil to:app];
    [app addItem:NSMenuItem.separatorItem];
    NSMenu* services = [NSMenu new];
    [self add:@"Services" action:nil key:nil to:app].submenu = services;
    NSApp.servicesMenu = services;
    [app addItem:NSMenuItem.separatorItem];
    [self add:@"Hide em34xx" action:@selector(hide:) key:@"h" to:app];
    [self add:@"Hide Others" action:@selector(hideOtherApplications:) key:@"h" to:app].keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [self add:@"Show All" action:@selector(unhideAllApplications:) key:nil to:app];
    [app addItem:NSMenuItem.separatorItem];
    [self add:@"Quit em34xx" action:@selector(terminate:) key:@"q" to:app];

    NSMenu* file = [self menu:@"File" in:bar];
    [self add:@"New Connection…" action:@selector(newConnection:) key:@"n" to:file];
    [self add:@"Connect to Profile…" action:@selector(connectToProfile:) key:@"o" to:file];
    [file addItem:NSMenuItem.separatorItem];
    [self add:@"Close Window" action:@selector(performClose:) key:@"w" to:file];
    [self add:@"Save" action:@selector(saveProfile:) key:@"s" to:file];
    [self add:@"Save As…" action:@selector(saveProfileAs:) key:@"S" to:file];
    [file addItem:NSMenuItem.separatorItem];
    [self add:@"Connection Settings…" action:@selector(editConnection:) key:@"i" to:file];
    [self add:@"Disconnect" action:@selector(toggleConnection:) key:@"d" to:file];

    // Standard editing commands, for the text fields of the dialogs.
    NSMenu* edit = [self menu:@"Edit" in:bar];
    [self add:@"Undo" action:@selector(undo:) key:@"z" to:edit];
    [self add:@"Redo" action:@selector(redo:) key:@"Z" to:edit];
    [edit addItem:NSMenuItem.separatorItem];
    [self add:@"Cut" action:@selector(cut:) key:@"x" to:edit];
    [self add:@"Copy" action:@selector(copy:) key:@"c" to:edit];
    [self add:@"Paste" action:@selector(paste:) key:@"v" to:edit];
    [self add:@"Select All" action:@selector(selectAll:) key:@"a" to:edit];

    NSMenu* view = [self menu:@"View" in:bar];
    [self add:@"Actual Size" action:@selector(zoomLevel:) key:@"0" to:view].tag = 100;
    [self add:@"Zoom 1.5×" action:@selector(zoomLevel:) key:nil to:view].tag = 150;
    [self add:@"Zoom 2×" action:@selector(zoomLevel:) key:nil to:view].tag = 200;
    [self add:@"Zoom In" action:@selector(zoomIn:) key:@"+" to:view];
    [self add:@"Zoom Out" action:@selector(zoomOut:) key:@"-" to:view];
    [view addItem:NSMenuItem.separatorItem];
    [self add:@"Show Toolbar" action:@selector(toggleToolbarShown:) key:@"t" to:view].keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [self add:@"Customize Toolbar…" action:@selector(runToolbarCustomizationPalette:) key:nil to:view];
    [view addItem:NSMenuItem.separatorItem];
    [self add:@"Enter Full Screen" action:@selector(toggleFullScreen:) key:@"f" to:view].keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagControl;

    NSMenu* term = [self menu:@"Terminal" in:bar];
    [self add:@"Attn" action:@selector(attn:) key:nil to:term];
    [self add:@"SysRq" action:@selector(sysRq:) key:nil to:term];
    [self add:@"Reset" action:@selector(terminalReset:) key:nil to:term];
    [self add:@"Setup" action:@selector(setup:) key:nil to:term];
    [term addItem:NSMenuItem.separatorItem];
    [self add:@"Reboot (Power Cycle)" action:@selector(reboot:) key:nil to:term];

    NSMenu* window = [self menu:@"Window" in:bar];
    [self add:@"Minimize" action:@selector(performMiniaturize:) key:@"m" to:window];
    [self add:@"Zoom" action:@selector(performZoom:) key:nil to:window];
    [window addItem:NSMenuItem.separatorItem];
    [self add:@"Bring All to Front" action:@selector(arrangeInFront:) key:nil to:window];
    NSApp.windowsMenu = window;

    NSMenu* help = [self menu:@"Help" in:bar];
    if (std::string(EM34XX_HOMEPAGE_URL).size())
        [self add:@"em34xx Project Page" action:@selector(projectPage:) key:nil to:help].target = self;
    [self add:@"Show Log in Finder" action:@selector(showLog:) key:nil to:help].target = self;
    NSApp.helpMenu = help;
    NSApp.mainMenu = bar;
}

- (void)applicationWillFinishLaunching:(NSNotification*)n {
    (void)n;
    [self buildMenus];
    // The battery-backed RAM is written at a clean exit.
    [NSProcessInfo.processInfo disableSuddenTermination];
    [NSProcessInfo.processInfo disableAutomaticTermination:@"the terminal is running"];
}

// .em34xx files double-clicked or dropped on the Dock icon: each opens in a
// window of its own (with the question for files from outside the profiles
// folder). Before the launch has finished they are kept for it.
- (void)application:(NSApplication*)sender openURLs:(NSArray<NSURL*>*)urls {
    (void)sender;
    for (NSURL* u in urls) {
        if (!u.fileURL) continue;
        const std::string path = str(u.path);
        if (!_launched) _pendingProfiles.push_back(path);
        else if (_launch) [self openPath:path foreign:true];
    }
}

- (void)applicationDidFinishLaunching:(NSNotification*)n {
    (void)n;
    guarded([&] {
        EMMainController* first = [self launch];
        _launched = true;
        if (!first) {
            [NSApp terminate:nil];
            return;
        }
        [first showTerminal];
        [NSApp activateIgnoringOtherApps:YES];
        // A block inside a [&] lambda does not capture self reliably: name it.
        EMAppDelegate* me = self;
        dispatch_async(dispatch_get_main_queue(), ^{
          guarded([me] { [me startup]; });
        });
    });
}

- (void)fail:(const std::string&)message detail:(const std::string&)detail code:(int)code {
    error_alert(nil, message, detail);
    _exitCode = code;
}

// The user folder, settings, the first-start ROM check and the keymap,
// then the first terminal. Nil: end.
- (EMMainController*)launch {
    _launch = std::make_unique<Launch>();
    Launch& L = *_launch;
    std::string root;
    if (const char* env = std::getenv("EM34XX_USER_DIR"); env && *env) {
        root = env;
    } else {
        const std::string support = application_support_folder();
        if (support.empty()) {
            [self fail:"Cannot find the Application Support folder." detail:"" code:2];
            return nil;
        }
        root = user_dirs_root(support);
    }
    L.dirs = std::make_unique<UserDirs>(root);
    std::string error;
    if (!L.dirs->ensure(error)) {
        [self fail:"Cannot create the user folder." detail:error code:2];
        return nil;
    }
    if (!load_settings(L.dirs->settings_file(), L.settings, error)) {
        error_alert(nil, "The settings cannot be read; default settings are used.", error);
        L.settings = AppSettings();
    }

    // Profiles from Finder, else from the command line (em34xx.app/Contents/
    // MacOS/em34xx PROFILE.em34xx ...). Arguments starting with '-' are left
    // to AppKit (-NSDocumentRevisionsDebugMode and the like).
    if (_pendingProfiles.empty()) {
        for (size_t i = 1; i < _argv.size(); ++i) {
            const std::string& a = _argv[i];
            if (a.empty() || a[0] == '-') {
                if (i + 1 < _argv.size() && a.rfind("-NS", 0) == 0) ++i;   // its value
                continue;
            }
            _pendingProfiles.push_back(a);
        }
    }

    // The first terminal starts as the profile that will be opened first
    // describes it (from Finder or the command line, else the startup
    // profile), else as a new profile would: the first model whose ROM
    // images are there, green, with scan lines.
    if (runnable_models().empty()) {
        [self fail:"No terminal model of this build can run yet." detail:"" code:2];
        return nil;
    }
    Profile start;
    std::string start_name;   // "" = no profile asks for a model
    const bool from_pending = !_pendingProfiles.empty();
    {
        std::string path = from_pending ? _pendingProfiles.front() : std::string();
        std::string why, ignored;   // load errors: reported when it is opened
        if (path.empty() && L.settings.startup_open && !L.settings.startup_profile.empty() &&
            UserDirs::valid_profile_name(L.settings.startup_profile, why) &&
            L.dirs->profile_exists(L.settings.startup_profile))
            path = L.dirs->profile_path(L.settings.startup_profile);
        if (!path.empty()) {
            if (load_profile(path, start, ignored)) start_name = stem_of(path);
            else start = Profile();
        }
    }

    // One complete set of ROM images is enough to start; the other models
    // stay selectable (greyed) until theirs are imported. A profile for a
    // model without its images: say which files, offer to import them, and
    // otherwise start without that profile.
    const std::string roms = L.dirs->roms();
    StartModel choice = choose_start_model(start_name.empty() ? std::string() : start.model, roms);
    bool declined = false;
    if (choice.wanted_unready) {
        declined = !ensure_model_roms(nil, *L.dirs, *choice.wanted_unready, "The profile \"" + start_name + "\"");
        choice = choose_start_model(start.model, roms);
        if (declined) {
            if (from_pending) _pendingProfiles.erase(_pendingProfiles.begin());
            else _skipStartupProfile = true;
            start = Profile();
        }
    }
    // First start (or ROMs gone): the ROM dialog, until one model can run.
    if (!choice.model && !declined && rom_manager_dialog(nil, *L.dirs, nullptr, true))
        choice = choose_start_model(start_name.empty() ? std::string() : start.model, roms);
    if (!choice.model) {
        [self fail:"em34xx needs the ROM images of at least one terminal model to start."
            detail:"Import them when you start em34xx again."
              code:1];
        return nil;
    }
    _model = choice.model;
    if (start.model != _model->info().name) {
        // Not the profile's model: this one's first variant and tube.
        start.model = _model->info().name;
        start.variant.clear();
        start.phosphor.clear();
    }

    // The global keymap, when there is one and it loads; else the built-in
    // Mac map (docs/keyboard.md).
    std::error_code ec;
    if (std::filesystem::exists(std::filesystem::u8path(L.dirs->keymap_file()), ec)) {
        std::string text;
        KeymapFile file;
        if (read_text_file(L.dirs->keymap_file(), text, error) &&
            parse_keymap_file(text, L.dirs->keymap_file(), file, error))
            L.keymap_path = L.dirs->keymap_file();
        else
            error_alert(nil, "The keymap file cannot be read; the default keymap is used.", error);
    }
    return [self makeTerminal:start primary:true];
}

// A terminal (its own Application: machine, emulation thread, sessions)
// built as `start` describes it, in a window not yet shown. Nil (with an
// alert) when it cannot start.
- (EMMainController*)makeTerminal:(const Profile&)start primary:(bool)primary {
    Launch& L = *_launch;
    // The profile's model when it can run now, else the one the app
    // started as (the profile's own model is asked for when it connects).
    const Model* model = _model;
    for (const Model* m : ready_models(L.dirs->roms()))
        if (m->info().name == start.model) model = m;
    const char* log_env = std::getenv("EM34XX_LOG");
    std::vector<std::string> args = {"em34xx",
                                     "--model",
                                     // The profile's variant of the model that starts;
                                     // without one, the model's first.
                                     start.model == model->info().name && !start.variant.empty()
                                         ? start.variant
                                         : model->info().name,
                                     "--rom-dir",
                                     L.dirs->roms(),
                                     "--speed",
                                     std::to_string(L.settings.speed_percent),
                                     "--keymap",
                                     L.keymap_path.empty() ? std::string("mac") : L.keymap_path,
                                     // Every terminal of a model shares its battery-backed RAM
                                     // file, as em34xxw instances do (docs/frontend-cocoa.md).
                                     "--nvram",
                                     L.dirs->nvram_file(model->info().name),
                                     // The setup EEPROM (monitor alignment values), likewise.
                                     "--eeprom",
                                     L.dirs->eeprom_file(model->info().name),
                                     // An end-user build keeps running on an access the model
                                     // does not decode (the default, stop, is for the debugger).
                                     "--unmapped",
                                     "log",
                                     "--log",
                                     log_env && *log_env ? std::string(log_env) : std::string("info")};
    // The log is one per process: ~/Library/Logs/em34xx.log (Console shows
    // it), or in the user folder when EM34XX_USER_DIR names one. Opened by
    // the first terminal; the others write to it too.
    if (!_logOpen) {
        std::string log_file = L.dirs->root() + "/em34xx.log";
        if (!std::getenv("EM34XX_USER_DIR")) {
            const std::string logs = logs_folder();
            if (!logs.empty()) log_file = logs + "/em34xx.log";
        }
        L.log_file = log_file;
        args.push_back("--log-file");
        args.push_back(log_file);
    }
    if (!start.crt) args.push_back("--no-crt");
    if (start.model == model->info().name && !start.phosphor.empty()) {
        args.push_back("--phosphor");
        args.push_back(start.phosphor);
    }

    auto fe = std::make_unique<CocoaFrontend>();
    CocoaFrontend* fe_ptr = fe.get();
    auto app = std::make_unique<Application>(std::move(fe), nullptr);
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(&a[0]);
    argv.push_back(nullptr);
    if (!app->start(int(args.size()), argv.data())) {
        const int code = app->exit_code();
        const std::string why = app->last_error();
        app->finish();
        [self fail:"The terminal cannot start." detail:why code:code ? code : 2];
        return nil;
    }
    _logOpen = true;
    EMMainController* t = [[EMMainController alloc] initWithApp:std::move(app)
                                                         launch:&L
                                                       frontend:fe_ptr
                                                        primary:primary];
    fe_ptr->controller = t;
    [_terminals addObject:t];
    return t;
}

- (void)terminalClosed:(EMMainController*)terminal {
    [_terminals removeObject:terminal];
}

// The terminal in front, if any: the frontmost terminal window (a sheet
// counts as its window), whether or not the app is active yet.
// The model a profile names, when it can run now (a model of this build
// with its ROM images; the user is told which are missing and may import
// them). Null, after an alert, otherwise.
- (const Model*)profileModel:(const Profile&)p what:(const std::string&)what {
    const Model* model = nullptr;
    for (const Model* m : runnable_models())
        if (m->info().name == p.model) model = m;
    if (!model) {
        error_alert([self dialogParent], what + " is for the IBM " + p.model + ".",
                    "This version of em34xx cannot emulate it.");
        return nullptr;
    }
    return ensure_model_roms([self dialogParent], *_launch->dirs, *model, what) ? model : nullptr;
}

- (EMMainController*)frontTerminal {
    for (NSWindow* w in NSApp.orderedWindows) {
        NSWindow* top = w;
        while (top.sheetParent) top = top.sheetParent;
        if (!top.visible && !top.miniaturized) continue;
        if ([top.windowController isKindOfClass:EMMainController.class] &&
            [_terminals containsObject:top.windowController])
            return top.windowController;
    }
    return nil;
}

- (NSWindow*)dialogParent {
    return [self frontTerminal].window;
}

// After the launch: the profiles Finder or the command line named, else the
// startup profile (Settings), else the profile manager over the first
// window. Cancel there leaves the terminal idle and not connected.
- (void)startup {
    LOG_DEBUG("ui", "startup: %zu profile(s) given", _pendingProfiles.size());
    if (_terminating || !_launch) return;
    if (!_pendingProfiles.empty()) {
        const std::vector<std::string> paths = std::move(_pendingProfiles);
        _pendingProfiles.clear();
        for (const std::string& p : paths) [self openPath:p foreign:true];
        return;
    }
    const AppSettings& s = _launch->settings;
    // The startup name is a file name in profiles/ and nothing else (it
    // comes from settings.toml): never a path.
    if (std::string why; !_skipStartupProfile && s.startup_open && !s.startup_profile.empty() &&
                         UserDirs::valid_profile_name(s.startup_profile, why) &&
                         _launch->dirs->profile_exists(s.startup_profile)) {
        [self openPath:_launch->dirs->profile_path(s.startup_profile) foreign:false];
    } else {
        [self connectToProfile:nil];
    }
}

// Where a profile opens: the terminal in front when it has none yet (the
// first window at start), else a new window. A profile already open in a
// window is shown there unless another session is asked for.
- (void)openProfile:(const Profile&)p path:(const std::string&)path untitled:(bool)untitled {
    LOG_DEBUG("ui", "open profile %s", untitled ? "(untitled)" : path.c_str());
    if (!untitled) {
        for (EMMainController* t in _terminals) {
            const std::string other = t.profilePath;
            std::error_code ec;
            if (other.empty() || !std::filesystem::equivalent(std::filesystem::u8path(other),
                                                              std::filesystem::u8path(path), ec))
                continue;
            NSAlert* a = [NSAlert new];
            a.messageText = ns("The profile \"" + stem_of(path) + "\" is already open.");
            a.informativeText = @"Show its window, or open another terminal with another session to the same "
                                @"systems.";
            [a addButtonWithTitle:@"Show Window"];
            [a addButtonWithTitle:@"Open Another Session"];
            [a addButtonWithTitle:@"Cancel"].keyEquivalent = @"\x1b";
            const NSModalResponse r = run_alert(a, [self dialogParent]);
            if (r == NSAlertFirstButtonReturn) {
                [t.window makeKeyAndOrderFront:nil];
                return;
            }
            if (r != NSAlertSecondButtonReturn) return;
            break;
        }
    }
    EMMainController* target = [self frontTerminal];
    if (!target || !target.idle) {
        LOG_DEBUG("ui", "the terminal in front is %s: a new window", target ? "in use" : "missing");
        target = [self makeTerminal:p primary:_terminals.count == 0];
        if (!target) return;
        [target showTerminal];
    }
    [target adoptProfile:p path:path untitled:untitled];
}

- (void)openPath:(const std::string&)path foreign:(bool)foreign {
    guarded([&] {
        Profile p;
        std::string error;
        if (!load_profile(path, p, error)) {
            error_alert([self dialogParent], "Cannot open the profile", error);
            return;
        }
        if (foreign && !file_in_folder(path, _launch->dirs->profiles()) &&
            !ask([self dialogParent], "This profile is not in your profiles folder. Connect?",
                 foreign_profile_details(path, p), "Connect", "Cancel", NSAlertStyleWarning, true))
            return;
        // A profile for a terminal whose ROM images are missing: the user
        // is told which files and may import them; otherwise nothing opens.
        if (![self profileModel:p what:"The profile \"" + stem_of(path) + "\""]) return;
        [self openProfile:p path:path untitled:false];
    });
}

// --- app-wide commands (the end of every window's responder chain) -----------------

- (void)newConnection:(id)sender {
    (void)sender;
    guarded([&] {
        EMMainController* front = [self frontTerminal];
        [front releaseKeys];
        Profile p;
        p.model = _model->info().name;
        if (!profile_editor_dialog(front.window, p, ProfileEditMode::Connect, _launch->settings, *_launch->dirs,
                                   "New Connection"))
            return;
        if (![self profileModel:p what:"This connection"]) return;
        [self openProfile:p path:"" untitled:true];
    });
}

- (void)connectToProfile:(id)sender {
    (void)sender;
    guarded([&] {
        EMMainController* front = [self frontTerminal];
        [front releaseKeys];
        const std::string name = profile_manager_dialog(front.window, *_launch->dirs, _launch->settings);
        if (!name.empty()) [self openPath:_launch->dirs->profile_path(name) foreign:false];
    });
}

- (void)showSettings:(id)sender {
    (void)sender;
    guarded([&] {
        [[self frontTerminal] releaseKeys];
        __weak EMAppDelegate* weak = self;
        show_settings_window(*_launch->dirs, _launch->settings, [weak](const AppSettings& before) {
            EMAppDelegate* d = weak;
            if (!d) return;
            const AppSettings& now = d->_launch->settings;
            if (now.speed_percent != before.speed_percent)
                for (EMMainController* t in d->_terminals) [t setSpeed:now.speed_percent];
            std::string error;
            if (!save_settings(d->_launch->dirs->settings_file(), now, error))
                error_alert(nil, "Cannot save the settings", error);
        });
    });
}

// The keymap is global: saved, it applies to every terminal at once.
- (void)editKeymap:(id)sender {
    (void)sender;
    guarded([&] {
        EMMainController* front = [self frontTerminal];
        [front releaseKeys];
        const UserDirs& dirs = *_launch->dirs;
        KeymapFile file = mac_keymap_file();
        std::string text, error;
        std::error_code ec;
        if (std::filesystem::exists(std::filesystem::u8path(dirs.keymap_file()), ec)) {
            if (!read_text_file(dirs.keymap_file(), text, error) ||
                !parse_keymap_file(text, dirs.keymap_file(), file, error)) {
                error_alert(front.window, "The keymap file cannot be read",
                            error + "\n\nThe keymap editor starts from the default keymap.");
                file = mac_keymap_file();
            }
        }
        if (!keymap_dialog(front.window, file, mac_keymap_file())) return;
        if (!write_text_file(dirs.keymap_file(),
                             keymap_file_to_toml(file, "em34xx host keymap (written by em34xx.app; "
                                                       "docs/keyboard.md).\nApplies to every profile."),
                             error)) {
            error_alert(front.window, "Cannot save the keymap", error);
            return;
        }
        _launch->keymap_path = dirs.keymap_file();
        for (EMMainController* t in _terminals) [t setKeymapSpec:_launch->keymap_path];
    });
}

- (void)about:(id)sender {
    (void)sender;
    if (_launch && _launch->dirs) guarded([&] { show_about_panel(*_launch->dirs); });
}

- (void)projectPage:(id)sender {
    (void)sender;
    [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@EM34XX_HOMEPAGE_URL]];
}

- (void)showLog:(id)sender {
    (void)sender;
    if (_launch && !_launch->log_file.empty()) reveal_in_finder(_launch->log_file);
}

- (BOOL)validateMenuItem:(NSMenuItem*)item {
    return [self validAction:item.action];
}

- (BOOL)validateToolbarItem:(NSToolbarItem*)item {
    return [self validAction:item.action];
}

- (BOOL)validAction:(SEL)action {
    if (action == @selector(about:) || action == @selector(projectPage:)) return YES;
    if (!_launch || !_model || _terminating) return NO;
    if (action == @selector(showLog:)) return !_launch->log_file.empty();
    // Not while a sheet or a modal dialog is up.
    if (NSApp.modalWindow) return NO;
    EMMainController* front = [self frontTerminal];
    if (front && front.window.attachedSheet) return NO;
    return YES;
}

// --- the Dock -----------------------------------------------------------------------

- (NSMenu*)applicationDockMenu:(NSApplication*)sender {
    (void)sender;
    NSMenu* m = [NSMenu new];
    [m addItemWithTitle:@"New Connection…" action:@selector(newConnection:) keyEquivalent:@""].target = self;
    [m addItemWithTitle:@"Connect to Profile…" action:@selector(connectToProfile:) keyEquivalent:@""].target = self;
    return m;
}

// The Dock icon clicked with no window open: a new terminal and the profile
// manager, as at start.
- (BOOL)applicationShouldHandleReopen:(NSApplication*)sender hasVisibleWindows:(BOOL)visible {
    (void)sender;
    if (visible || !_launched || !_launch || _terminals.count || NSApp.modalWindow) return YES;
    guarded([&] {
        Profile blank;
        blank.model = _model->info().name;
        EMMainController* t = [self makeTerminal:blank primary:true];
        if (!t) return;
        [t showTerminal];
        EMAppDelegate* me = self;
        dispatch_async(dispatch_get_main_queue(), ^{
          guarded([me] { [me connectToProfile:nil]; });
        });
    });
    return NO;
}

// --- quitting -------------------------------------------------------------------------

// Quit, log-out and shutdown: every edited profile is asked about (its
// window in front); then every terminal winds down as at a clean exit
// (sessions closed, battery-backed RAM written).
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    (void)sender;
    for (EMMainController* t in [_terminals copy]) {
        if (t.quitting) continue;
        [t.window makeKeyAndOrderFront:nil];
        if (![t confirmDiscard]) return NSTerminateCancel;
    }
    _terminating = true;
    for (EMMainController* t in [_terminals copy]) {
        t.quitting = true;
        [t shutdownTerminal];
        [t.window orderOut:nil];
    }
    [_terminals removeAllObjects];
    return NSTerminateNow;
}

// The program keeps running with no window, as Mac apps do: File > New
// Connection, Connect to Profile and the Dock icon open a terminal again.
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    (void)sender;
    return NO;
}

- (BOOL)applicationSupportsSecureRestorableState:(NSApplication*)app {
    (void)app;
    return YES;
}

- (void)applicationWillResignActive:(NSNotification*)n {
    (void)n;
    for (EMMainController* t in _terminals) [t releaseKeys];
}

@end

// ===========================================================================

int em34xx::cocoa_main(int argc, char** argv) {
    @autoreleasepool {
        std::vector<std::string> args(argv, argv + argc);
        NSApplication* app = NSApplication.sharedApplication;
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        EMAppDelegate* delegate = [[EMAppDelegate alloc] initWithArgs:args];
        app.delegate = delegate;
        // SIGTERM (kill, launchctl) ends the program as a quit without
        // questions would: the battery-backed RAM is still written.
        signal(SIGTERM, SIG_IGN);
        dispatch_source_t term =
            dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_main_queue());
        dispatch_source_set_event_handler(term, ^{
          [delegate quitWithoutQuestions];
        });
        dispatch_resume(term);
        [app run];
        return [delegate exitCode];
    }
}
