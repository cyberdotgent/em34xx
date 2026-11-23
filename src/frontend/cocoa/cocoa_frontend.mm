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
    std::string profile_path;   // a .em34xx file from Finder or the command line
    bool profile_foreign_check = true;
    std::vector<std::string> args;
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

@interface EMMainController : NSObject <NSWindowDelegate, NSToolbarDelegate, NSMenuItemValidation,
                                        NSToolbarItemValidation>
- (instancetype)initWithApp:(Application*)app launch:(Launch*)launch frontend:(CocoaFrontend*)fe;
- (void)showAndStart;
- (void)openProfileFile:(const std::string&)path foreign:(bool)foreign;
- (bool)confirmDiscard;
- (void)shutdown;
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
@property(readonly) NSWindow* window;
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
    Application* _app;
    Emulator* _emu;
    Launch* _launch;
    CocoaFrontend* _fe;
    std::string _modelName;
    NSWindow* _window;
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

@synthesize window = _window;

- (instancetype)initWithApp:(Application*)app launch:(Launch*)launch frontend:(CocoaFrontend*)fe {
    if ((self = [super init])) {
        _app = app;
        _emu = &app->emulator();
        _launch = launch;
        _fe = fe;
        _modelName = app->model().info().name;
        _crtParams = app->frontend_options().crt;
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
    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 800, 600)
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                    NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    _window.releasedWhenClosed = NO;
    _window.delegate = self;
    _window.title = @"em34xx";
    _window.tabbingMode = NSWindowTabbingModeDisallowed;
    _window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    _window.contentMinSize = NSMakeSize(320, 240);
    if (@available(macOS 11.0, *)) _window.toolbarStyle = NSWindowToolbarStyleUnified;

    NSToolbar* tb = [[NSToolbar alloc] initWithIdentifier:@"em34xx.terminal"];
    tb.delegate = self;
    tb.allowsUserCustomization = YES;
    tb.autosavesConfiguration = YES;
    tb.displayMode = NSToolbarDisplayModeIconOnly;
    _window.toolbar = tb;

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
    _window.contentView = root;
    _window.initialFirstResponder = _view;
}

// Content size: the presented frame times the zoom (in points: a Retina
// screen shows it with twice the pixels), plus the status bar; shrunk to
// the screen when it does not fit. The top-left corner stays put.
- (void)sizeForZoom {
    if (_window.styleMask & NSWindowStyleMaskFullScreen) return;
    NSScreen* screen = _window.screen ? _window.screen : NSScreen.mainScreen;
    const NSRect visible = screen.visibleFrame;
    double scale = [self settings].zoom;
    NSRect frame = NSZeroRect;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const NSSize content = NSMakeSize(std::round(_baseW * scale), std::round(_baseH * scale) + kStatusBarHeight);
        frame = [_window frameRectForContentRect:NSMakeRect(0, 0, content.width, content.height)];
        const double fit = std::min(visible.size.width / frame.size.width, visible.size.height / frame.size.height);
        if (fit >= 1.0) break;
        scale *= fit * 0.98;
    }
    const NSRect old = _window.frame;
    frame.origin.x = old.origin.x;
    frame.origin.y = NSMaxY(old) - frame.size.height;
    frame.origin.x = std::min(std::max(frame.origin.x, visible.origin.x), NSMaxX(visible) - frame.size.width);
    frame.origin.y = std::min(std::max(frame.origin.y, visible.origin.y), NSMaxY(visible) - frame.size.height);
    [_window setFrame:frame display:YES animate:_window.visible];
}

- (void)showAndStart {
    // The last frame (NSUserDefaults), or the zoom size centred.
    if (![_window setFrameUsingName:@"em34xx.terminal"]) {
        [self sizeForZoom];
        [_window center];
    }
    _window.frameAutosaveName = @"em34xx.terminal";

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
    [_window makeKeyAndOrderFront:nil];
    __weak EMMainController* weak = self;
    _statusTimer = [NSTimer timerWithTimeInterval:0.5
                                          repeats:YES
                                            block:^(NSTimer*) {
                                              [weak statusTick];
                                            }];
    [NSRunLoop.mainRunLoop addTimer:_statusTimer forMode:NSRunLoopCommonModes];

    // Once the window is on screen: the profile Finder or the command line
    // named, else the startup profile, else the profile manager.
    dispatch_async(dispatch_get_main_queue(), ^{
      EMMainController* c = weak;
      if (c) guarded([&] { [c startup]; });
    });
}

- (void)startup {
    if (_quitting) return;
    if (!_launch->profile_path.empty()) {
        [self openProfileFile:_launch->profile_path foreign:_launch->profile_foreign_check];
        return;
    }
    // The startup name is a file name in profiles/ and nothing else (it
    // comes from settings.toml): never a path.
    const AppSettings& s = [self settings];
    if (std::string why; s.startup_open && !s.startup_profile.empty() &&
                         UserDirs::valid_profile_name(s.startup_profile, why) &&
                         [self dirs].profile_exists(s.startup_profile)) {
        [self openProfileFile:[self dirs].profile_path(s.startup_profile) foreign:false];
    } else {
        [self connectToProfile:nil];
    }
}

- (void)statusTick {
    guarded([&] {
        if (_emu->quit_requested()) {
            [self quitRequested];
            return;
        }
        [self handleTrustRequests];   // a prompt that waited for another dialog to close
        [self updateStatus];
    });
}

- (void)shutdown {
    [_statusTimer invalidate];
    _statusTimer = nil;
    [_rasterTimer invalidate];
    _rasterTimer = nil;
    _trust->close();
    _emu->set_display_callback(nullptr);
    _app->host_link().set_tls_trust_handler(nullptr);
    std::string error;
    save_settings([self dirs].settings_file(), [self settings], error);
}

- (void)quitRequested {
    if (_quitting) return;
    _quitting = true;
    _cur.modified = false;   // asked to end: no questions
    [NSApp terminate:nil];
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
    if (!_dirty || !_window) return;
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
    guarded([&] {
        // Command shortcuts belong to the Mac (menus); the terminal does its
        // own typematic, so the Mac's auto-repeats are dropped.
        if (down && ((e.modifierFlags & NSEventModifierFlagCommand) || e.ARepeat)) return;
        const uint32_t hid = hid_from_keycode(e.keyCode);
        if (hid) [self sendKey:hid down:down];
    });
}

- (void)flagsEvent:(NSEvent*)e {
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
    if (_held.empty()) return;
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
    [_window makeFirstResponder:_view];
}

- (void)windowDidResignKey:(NSNotification*)n {
    (void)n;
    guarded([&] { [self releaseKeys]; });
}

- (void)windowDidBecomeKey:(NSNotification*)n {
    (void)n;
    if (!_window.attachedSheet) [_window makeFirstResponder:_view];
}

// --- title and status ---------------------------------------------------------------

- (std::string)profileName {
    return _cur.untitled ? std::string("Untitled Profile") : _cur.name;
}

- (void)titleChanged {
    const std::string override_title = _fe->title(), note = _fe->status();
    _window.title = _cur.active ? ns([self profileName]) : @"em34xx";
    std::string sub = override_title.empty() ? "IBM " + _modelName : override_title;
    if (!note.empty()) sub += " — " + note;
    if (@available(macOS 11.0, *)) _window.subtitle = ns(sub);
    _window.documentEdited = _cur.active && _cur.modified;
    _window.representedURL =
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
    [_window.toolbar validateVisibleItems];
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
    it.target = self;
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
                                                 target:self
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

- (DisplaySubtype)currentDisplay {
    return _emu->with_machine([](Machine& m) { return m.options().display; });
}

// Apply the profile's terminal settings (display subtype, scan lines).
// `rebuilt`: the change was visible to the firmware or the host, so the
// machine was rebuilt and is powering on (its sessions closed).
- (bool)applyTerminalSettings:(const Profile&)p rebuilt:(bool&)rebuilt {
    rebuilt = false;
    if (_crtParams.enabled != p.crt) {
        _crtParams.enabled = p.crt;
        _app->set_crt(_crtParams);
        _crt.invalidate();
    }
    if ([self currentDisplay] != p.display) {
        rebuilt = _app->display_change_needs_power_cycle(p.display);
        std::optional<TrustDrain> drain;   // the rebuild closes the sessions
        if (rebuilt) {
            drain.emplace(*_trust);
            [self releaseKeys];
        }
        std::string error;
        const bool ok = _app->set_display_subtype(p.display, error);
        drain.reset();
        if (!ok) {
            rebuilt = false;
            error_alert(_window, "Cannot change the CRT type", error);
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
    if (p.model != _modelName) {
        error_alert(_window, "This profile is for the IBM " + p.model + ".",
                    "The emulator runs an IBM " + _modelName + "; other models cannot run in this build yet.");
        return;
    }
    AppConfig cfg;
    std::string error, message;
    if (!profile_connect_config(p, _app->host_link().base(), [self settings].tls_min_version, cfg, error)) {
        error_alert(_window, "Cannot connect", error);
        return;
    }
    bool rebuilt = false;
    {
        // The running sessions close with the trust queue draining (their
        // threads are joined); the new sessions may ask again.
        TrustDrain drain(*_trust);
        [self applyTerminalSettings:p rebuilt:rebuilt];
        [self releaseKeys];
        std::string ignored;
        _app->host_link().disconnect(ignored);   // "not connected" is fine
    }
    _cur.accepted_once = false;
    for (bool& o : _cur.overridden) o = false;
    const bool ok = powerCycle && !rebuilt ? _app->connect_after_power_cycle(cfg, message, error)
                                           : _app->host_link().connect(cfg, message, error);
    if (!ok) {
        error_alert(_window, "Cannot connect", error);
        [self updateUi];
        return;
    }
    LOG_INFO("tn5250", "%s", trim(message).c_str());
    [self updateUi];
}

- (void)newConnection:(id)sender {
    (void)sender;
    guarded([&] {
        [self releaseKeys];
        Profile p;
        p.model = _modelName;
        if (!profile_editor_dialog(_window, p, ProfileEditMode::Connect, [self settings], "New Connection")) return;
        if (![self confirmDiscard]) return;
        _cur = CurrentProfile();
        _cur.active = true;
        _cur.p = p;
        _cur.untitled = true;
        _cur.modified = true;   // never saved
        [self connectProfile:_cur.p powerCycle:true];
    });
}

// File > Connection Settings: the running profile in the editor. Changes
// mark it edited and apply now: the CRT type at once (or after a
// confirmation when the terminal must restart), connection changes by
// reconnecting the sessions with the terminal running.
- (void)editConnection:(id)sender {
    guarded([&] {
        if (!_cur.active) {
            [self newConnection:sender];
            return;
        }
        [self releaseKeys];
        Profile p = _cur.p;
        if (!profile_editor_dialog(_window, p, ProfileEditMode::Apply, [self settings],
                                   "Connection Settings: " + [self profileName]))
            return;
        if (p == _cur.p) return;
        const Profile old = _cur.p;
        if (p.display != [self currentDisplay] && _app->display_change_needs_power_cycle(p.display)) {
            std::string label = subtype_label(p.display);
            if (!label.empty()) label[0] = char(std::tolower(static_cast<unsigned char>(label[0])));
            if (!ask(_window, "Change the CRT type to " + label + "?",
                     "This restarts the terminal and reconnects the sessions.", "Restart", "Keep the CRT Type",
                     NSAlertStyleWarning))
                p.display = old.display;   // keep the rest of the changes
        }
        Profile same_terminal = old;
        same_terminal.display = p.display;
        same_terminal.crt = p.crt;
        const bool connection_changed = same_terminal != p;
        const bool was_connected = [self connected];
        bool rebuilt = false;
        if (![self applyTerminalSettings:p rebuilt:rebuilt]) p.display = [self currentDisplay];
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
        [_window makeFirstResponder:_view];
    });
}

- (void)reboot:(id)sender {
    (void)sender;
    guarded([&] {
        [self releaseKeys];
        _app->power_on();
        [_window makeFirstResponder:_view];
    });
}

- (void)openProfileFile:(const std::string&)path foreign:(bool)foreign {
    guarded([&] {
        Profile p;
        std::string error;
        if (!load_profile(path, p, error)) {
            error_alert(_window, "Cannot open the profile", error);
            return;
        }
        if (foreign && !file_in_folder(path, [self dirs].profiles()) &&
            !ask(_window, "This profile is not in your profiles folder. Connect?", foreign_profile_details(path, p),
                 "Connect", "Cancel", NSAlertStyleWarning, true))
            return;
        if (![self confirmDiscard]) return;
        _cur = CurrentProfile();
        _cur.active = true;
        _cur.p = p;
        _cur.name = stem_of(path);
        _cur.path = path;
        [self connectProfile:_cur.p powerCycle:true];
        [_window makeFirstResponder:_view];
    });
}

- (void)connectToProfile:(id)sender {
    (void)sender;
    guarded([&] {
        [self releaseKeys];
        const std::string name = profile_manager_dialog(_window, [self dirs], [self settings]);
        if (!name.empty()) [self openProfileFile:[self dirs].profile_path(name) foreign:false];
        [_window makeFirstResponder:_view];
    });
}

// Save (or Save As) the current profile. False when cancelled or failed.
- (bool)saveCurrent:(bool)saveAs {
    if (!_cur.active) return false;
    const UserDirs& dirs = [self dirs];
    if (_cur.untitled || saveAs) {
        std::string name = _cur.untitled ? dirs.unique_profile_name(_cur.p.sessions[0].host) : _cur.name;
        if (!name_dialog(_window, "Save Profile As", "Name of the profile:", "Save", name)) return false;
        if (dirs.profile_exists(name) && (_cur.untitled || name != _cur.name) &&
            !ask(_window, "A profile named \"" + name + "\" already exists. Replace it?",
                 "Replacing it overwrites its settings.", "Replace", "Cancel", NSAlertStyleWarning, true))
            return false;
        std::string error;
        if (!save_profile(dirs.profile_path(name), _cur.p, error)) {
            error_alert(_window, "Cannot save the profile", error);
            return false;
        }
        _cur.name = name;
        _cur.path = dirs.profile_path(name);
        _cur.untitled = false;
        refresh_settings_window();
    } else {
        std::string error;
        if (!save_profile(_cur.path, _cur.p, error)) {
            error_alert(_window, "Cannot save the profile", error);
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
    const NSModalResponse r = run_alert(a, _window);
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
        if (_trust->empty() || modal_ui_active(_window)) return;
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
                d = certificate_invalid_dialog(_window, p->req) ? tn5250::TlsTrustDecision::AcceptOnce
                                                                : tn5250::TlsTrustDecision::Reject;
                if (p->req.station < Profile::kSessions)
                    _cur.overridden[p->req.station] = d == tn5250::TlsTrustDecision::AcceptOnce;
            } else {
                d = trust_dialog(_window, p->req, name);
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
                    if (!error.empty()) error_alert(_window, "Cannot save the profile", error);
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
        [_window makeFirstResponder:_view];
    });
}

// --- keymap, settings, view, terminal ---------------------------------------------

- (void)editKeymap:(id)sender {
    (void)sender;
    guarded([&] {
        [self releaseKeys];
        const UserDirs& dirs = [self dirs];
        KeymapFile file = mac_keymap_file();
        std::string text, error;
        std::error_code ec;
        if (std::filesystem::exists(std::filesystem::u8path(dirs.keymap_file()), ec)) {
            if (!read_text_file(dirs.keymap_file(), text, error) ||
                !parse_keymap_file(text, dirs.keymap_file(), file, error)) {
                error_alert(_window, "The keymap file cannot be read",
                            error + "\n\nThe keymap editor starts from the default keymap.");
                file = mac_keymap_file();
            }
        }
        if (!keymap_dialog(_window, file, mac_keymap_file())) return;
        if (!write_text_file(dirs.keymap_file(),
                             keymap_file_to_toml(file, "em34xx host keymap (written by em34xx.app; "
                                                       "docs/keyboard.md).\nApplies to every profile."),
                             error)) {
            error_alert(_window, "Cannot save the keymap", error);
            return;
        }
        _launch->keymap_path = dirs.keymap_file();
        const std::string spec = _launch->keymap_path;
        if (!_emu->with_machine([&](Machine& m) { return m.set_keymap(spec, error); }))
            error_alert(_window, "The keymap was saved but could not be applied", error);
        [_window makeFirstResponder:_view];
    });
}

- (void)showSettings:(id)sender {
    (void)sender;
    guarded([&] {
        [self releaseKeys];
        __weak EMMainController* weak = self;
        show_settings_window([self dirs], [self settings], [weak](const AppSettings& before) {
            EMMainController* c = weak;
            if (!c) return;
            const AppSettings& now = [c settings];
            if (now.speed_percent != before.speed_percent) c->_app->set_speed_percent(now.speed_percent);
            std::string error;
            if (!save_settings([c dirs].settings_file(), now, error))
                error_alert(nil, "Cannot save the settings", error);
        });
    });
}

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

// Closing the window ends the program (a single-window app): the questions
// are asked by applicationShouldTerminate:.
- (BOOL)windowShouldClose:(NSWindow*)w {
    (void)w;
    [NSApp terminate:nil];
    return NO;
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

@interface EMAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation EMAppDelegate {
    std::unique_ptr<Launch> _launch;
    std::unique_ptr<Application> _app;
    EMMainController* _controller;
    std::vector<std::string> _argv;
    std::string _pendingProfile;   // opened from Finder before the window was up
    bool _launched;
    int _exitCode;
}

- (instancetype)initWithArgs:(std::vector<std::string>)args {
    if ((self = [super init])) {
        _argv = std::move(args);
        _launched = false;
        _exitCode = 0;
    }
    return self;
}

- (int)exitCode {
    return _exitCode;
}

- (void)quitWithoutQuestions {
    if (_controller) [_controller quitRequested];
    else [NSApp terminate:nil];
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

// A .em34xx file double-clicked or dropped on the Dock icon: before the
// launch it becomes the startup profile; afterwards it is opened (with the
// question for files from outside the profiles folder).
- (void)application:(NSApplication*)sender openURLs:(NSArray<NSURL*>*)urls {
    (void)sender;
    NSURL* first = urls.firstObject;
    if (!first.fileURL) return;
    const std::string path = str(first.path);
    if (!_launched) {
        _pendingProfile = path;
        return;
    }
    if (_controller) [_controller openProfileFile:path foreign:true];
}

- (void)applicationDidFinishLaunching:(NSNotification*)n {
    (void)n;
    guarded([&] {
        if (![self launch]) {
            _launched = true;
            [NSApp terminate:nil];
            return;
        }
        _launched = true;
        [_controller showAndStart];
        [NSApp activateIgnoringOtherApps:YES];
    });
}

- (void)fail:(const std::string&)message detail:(const std::string&)detail code:(int)code {
    error_alert(nil, message, detail);
    _exitCode = code;
}

// The user folder, settings, the profile to open, the first-start ROM
// check, the keymap, then the Application and the window. False: end.
- (bool)launch {
    _launch = std::make_unique<Launch>();
    Launch& L = *_launch;
    std::string root;
    if (const char* env = std::getenv("EM34XX_USER_DIR"); env && *env) {
        root = env;
    } else {
        const std::string support = application_support_folder();
        if (support.empty()) {
            [self fail:"Cannot find the Application Support folder." detail:"" code:2];
            return false;
        }
        root = user_dirs_root(support);
    }
    L.dirs = std::make_unique<UserDirs>(root);
    std::string error;
    if (!L.dirs->ensure(error)) {
        [self fail:"Cannot create the user folder." detail:error code:2];
        return false;
    }
    if (!load_settings(L.dirs->settings_file(), L.settings, error)) {
        error_alert(nil, "The settings cannot be read; default settings are used.", error);
        L.settings = AppSettings();
    }

    // The profile: from Finder, else the command line (em34xx.app/Contents/
    // MacOS/em34xx PROFILE.em34xx). Arguments starting with '-' are left to
    // AppKit (-NSDocumentRevisionsDebugMode and the like).
    if (!_pendingProfile.empty()) {
        L.profile_path = _pendingProfile;
    } else {
        for (size_t i = 1; i < _argv.size(); ++i) {
            const std::string& a = _argv[i];
            if (a.empty() || a[0] == '-') {
                if (i + 1 < _argv.size() && a.rfind("-NS", 0) == 0) ++i;   // its value
                continue;
            }
            if (L.profile_path.empty()) L.profile_path = a;
        }
    }

    // The terminal starts as the profile describes it, else as a new
    // profile would: the first runnable model, green, with scan lines.
    const auto models = runnable_models();
    if (models.empty()) {
        [self fail:"No terminal model of this build can run yet." detail:"" code:2];
        return false;
    }
    Profile start;
    if (!L.profile_path.empty()) {
        std::string ignored;   // reported when the window opens it
        if (!load_profile(L.profile_path, start, ignored)) start = Profile();
    }
    const Model* model = models.front();
    for (const Model* m : models)
        if (m->info().name == start.model) model = m;

    // First start (or ROMs gone): the ROM manager.
    if (!model_roms_ready(*model, L.dirs->roms()) && !rom_manager_dialog(nil, *L.dirs, model)) {
        [self fail:"em34xx cannot start the IBM " + model->info().name + " without its ROM images."
            detail:"Import them when you start em34xx again."
              code:1];
        return false;
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

    // The log: ~/Library/Logs/em34xx.log (Console shows it), or in the
    // user folder when EM34XX_USER_DIR names one.
    std::string log_file = root + "/em34xx.log";
    if (!std::getenv("EM34XX_USER_DIR")) {
        const std::string logs = logs_folder();
        if (!logs.empty()) log_file = logs + "/em34xx.log";
    }
    const char* log_env = std::getenv("EM34XX_LOG");
    L.args = {"em34xx",
              "--model",
              model->info().name,
              "--rom-dir",
              L.dirs->roms(),
              "--display",
              display_subtype_name(start.display),
              "--speed",
              std::to_string(L.settings.speed_percent),
              "--keymap",
              L.keymap_path.empty() ? std::string("mac") : L.keymap_path,
              "--nvram",
              L.dirs->nvram_file(model->info().name),
              // An end-user build keeps running on an access the model does
              // not decode (the default, stop, is for the debugger).
              "--unmapped",
              "log",
              "--log",
              log_env && *log_env ? std::string(log_env) : std::string("info"),
              "--log-file",
              log_file};
    if (!start.crt) L.args.push_back("--no-crt");

    auto fe = std::make_unique<CocoaFrontend>();
    CocoaFrontend* fe_ptr = fe.get();
    _app = std::make_unique<Application>(std::move(fe), nullptr);
    std::vector<char*> argv;
    for (std::string& a : L.args) argv.push_back(&a[0]);
    argv.push_back(nullptr);
    if (!_app->start(int(L.args.size()), argv.data())) {
        const int code = _app->exit_code();
        const std::string why = _app->last_error();
        _app->finish();
        _app.reset();
        [self fail:"The terminal cannot start." detail:why code:code ? code : 2];
        return false;
    }
    _controller = [[EMMainController alloc] initWithApp:_app.get() launch:_launch.get() frontend:fe_ptr];
    fe_ptr->controller = _controller;
    return true;
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    (void)sender;
    if (!_controller) return NSTerminateNow;
    if (!_controller.quitting && ![_controller confirmDiscard]) return NSTerminateCancel;
    // As at a clean exit: settings, then the sessions and the emulation
    // thread wind down and the battery-backed RAM is written (finish()).
    // Also at log-out and shutdown, which come through here.
    _controller.quitting = true;
    guarded([&] {
        [_controller shutdown];
        _app->request_quit(0);
        _exitCode = _app->finish();
        [_controller.window orderOut:nil];
        _app.reset();
    });
    _controller = nil;
    return NSTerminateNow;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    (void)sender;
    return YES;
}

- (BOOL)applicationSupportsSecureRestorableState:(NSApplication*)app {
    (void)app;
    return YES;
}

- (void)applicationWillResignActive:(NSNotification*)n {
    (void)n;
    [_controller releaseKeys];
}

// --- actions the delegate answers itself (the main controller is not in
// the responder chain: it is forwarded to) ---------------------------------------

- (id)forwardingTargetForSelector:(SEL)sel {
    if (_controller && [_controller respondsToSelector:sel]) return _controller;
    return [super forwardingTargetForSelector:sel];
}

- (BOOL)respondsToSelector:(SEL)sel {
    if ([super respondsToSelector:sel]) return YES;
    return _controller && [_controller respondsToSelector:sel] &&
           ![NSStringFromSelector(sel) hasPrefix:@"application"] && ![NSStringFromSelector(sel) hasPrefix:@"window"];
}

- (BOOL)validateMenuItem:(NSMenuItem*)item {
    if (item.action == @selector(about:) || item.action == @selector(projectPage:) ||
        item.action == @selector(showLog:))
        return YES;
    if (!_controller) return NO;
    // No terminal commands while a sheet or modal dialog is up.
    if (modal_ui_active(_controller.window) && item.action != @selector(showSettings:)) return NO;
    return [_controller validateMenuItem:item];
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
    std::string log;
    if (_launch) {
        for (size_t i = 0; i + 1 < _launch->args.size(); ++i)
            if (_launch->args[i] == "--log-file") log = _launch->args[i + 1];
    }
    if (!log.empty()) reveal_in_finder(log);
}

@end

// ===========================================================================

int em34xx::cocoa_main(int argc, char** argv) {
    @autoreleasepool {
        std::vector<std::string> args(argv, argv + argc);
        NSApplication* app = NSApplication.sharedApplication;
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        NSWindow.allowsAutomaticWindowTabbing = NO;
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
