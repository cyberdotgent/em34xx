// SPDX-License-Identifier: GPL-3.0-or-later
#import "frontend/cocoa/cocoa_dialogs.h"

#import "frontend/cocoa/cocoa_util.h"

#include "em34xx_version.h"
#include "models/model.hpp"
#include "tn5250/tls_policy.hpp"
#include "util/fingerprint.hpp"
#include "util/strings.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <thread>

using namespace em34xx;
using namespace em34xx::cocoa;

namespace {

const unsigned kSpeeds[] = {25, 50, 100, 200, 400, 0};
const TlsVersion kTlsVersions[] = {TlsVersion::Ssl3, TlsVersion::Tls1_0, TlsVersion::Tls1_1, TlsVersion::Tls1_2,
                                   TlsVersion::Tls1_3};
constexpr size_t kTlsVersionCount = sizeof kTlsVersions / sizeof kTlsVersions[0];
constexpr size_t kSpeedCount = sizeof kSpeeds / sizeof kSpeeds[0];

std::string tls_version_label(TlsVersion v) {
    switch (v) {
    case TlsVersion::Ssl3: return "SSL 3.0 (broken)";
    case TlsVersion::Tls1_0: return "TLS 1.0 (legacy)";
    case TlsVersion::Tls1_1: return "TLS 1.1 (legacy)";
    case TlsVersion::Tls1_2: return "TLS 1.2";
    case TlsVersion::Tls1_3: return "TLS 1.3";
    case TlsVersion::Any: break;
    }
    return "?";
}

// One "Label:   value" line per field, aligned for a monospaced view.
std::string field(const std::string& name, const std::string& value) {
    std::string n = name + ":";
    if (n.size() < 14) n.resize(14, ' ');
    return n + value + "\n";
}

std::string certificate_text(const tn5250::PeerCertificate& c) {
    std::string t;
    t += field("Subject", c.subject);
    t += field("Issuer", c.issuer);
    t += field("Valid from", c.valid_from.empty() ? std::string("?") : c.valid_from);
    t += field("Valid until", c.expires + (c.expired ? "  (EXPIRED)" : ""));
    t += field("SHA-256", c.have_digest ? format_sha256_fingerprint(c.sha256) : std::string("unavailable"));
    t += field("Verification", c.chain_ok ? std::string("OK") : "failed: " + c.chain_error);
    // Validity period and revocation, apart from trust: a pin does not
    // settle these.
    for (const std::string& e : tn5250::tls_validity_errors(c)) t += field("NOT VALID", e);
    if (!c.revoked)
        t += field("Revocation", c.revocation_checked ? std::string("not revoked")
                                                      : "not checked" + (c.revocation_note.empty()
                                                                             ? std::string()
                                                                             : " (" + c.revocation_note + ")"));
    return t;
}

void set_text(NSTextView* tv, const std::string& text) { tv.string = ns(text); }

// A form: labels right-aligned in the first column, controls in the second.
NSGridView* form(NSArray<NSArray<NSView*>*>* rows) {
    NSGridView* g = [NSGridView gridViewWithViews:rows];
    g.translatesAutoresizingMaskIntoConstraints = NO;
    g.rowSpacing = 8;
    g.columnSpacing = 8;
    [g columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;
    g.rowAlignment = NSGridRowAlignmentFirstBaseline;
    return g;
}

NSView* empty() { return [NSGridCell emptyContentView]; }

NSBox* group(NSString* title, NSView* content) {
    NSBox* b = [NSBox new];
    b.translatesAutoresizingMaskIntoConstraints = NO;
    b.title = title;
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [b.contentView addSubview:content];
    [NSLayoutConstraint activateConstraints:@[
        [content.topAnchor constraintEqualToAnchor:b.contentView.topAnchor constant:8],
        [content.leadingAnchor constraintEqualToAnchor:b.contentView.leadingAnchor constant:10],
        [content.trailingAnchor constraintLessThanOrEqualToAnchor:b.contentView.trailingAnchor constant:-10],
        [content.bottomAnchor constraintEqualToAnchor:b.contentView.bottomAnchor constant:-10],
    ]];
    return b;
}

NSTableColumn* column(NSString* ident, NSString* title, CGFloat width) {
    NSTableColumn* c = [[NSTableColumn alloc] initWithIdentifier:ident];
    c.title = title;
    c.width = width;
    c.resizingMask = NSTableColumnUserResizingMask | NSTableColumnAutoresizingMask;
    return c;
}

NSScrollView* table_in_scroll(NSTableView* table, CGFloat width, CGFloat height) {
    NSScrollView* sv = [NSScrollView new];
    sv.translatesAutoresizingMaskIntoConstraints = NO;
    sv.documentView = table;
    sv.hasVerticalScroller = YES;
    sv.borderType = NSBezelBorder;
    [sv.widthAnchor constraintGreaterThanOrEqualToConstant:width].active = YES;
    [sv.heightAnchor constraintGreaterThanOrEqualToConstant:height].active = YES;
    table.usesAlternatingRowBackgroundColors = YES;
    table.allowsMultipleSelection = NO;
    table.allowsEmptySelection = YES;
    if (@available(macOS 11.0, *)) table.style = NSTableViewStyleFullWidth;
    return sv;
}

// The text of a table cell, reusing cell views.
NSView* cell_text(NSTableView* table, NSTableColumn* col, const std::string& text) {
    NSTableCellView* cell = [table makeViewWithIdentifier:col.identifier owner:nil];
    if (!cell) {
        cell = [NSTableCellView new];
        cell.identifier = col.identifier;
        NSTextField* t = [NSTextField labelWithString:@""];
        t.translatesAutoresizingMaskIntoConstraints = NO;
        t.lineBreakMode = NSLineBreakByTruncatingTail;
        [cell addSubview:t];
        cell.textField = t;
        [NSLayoutConstraint activateConstraints:@[
            [t.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor constant:2],
            [t.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-2],
            [t.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
        ]];
    }
    cell.textField.stringValue = ns(text);
    return cell;
}

void select_row(NSTableView* t, NSInteger row) {
    if (row < 0 || row >= t.numberOfRows) {
        [t deselectAll:nil];
        return;
    }
    [t selectRowIndexes:[NSIndexSet indexSetWithIndex:NSUInteger(row)] byExtendingSelection:NO];
    [t scrollRowToVisible:row];
}

std::vector<std::string> tls_version_items(const std::string& first) {
    std::vector<std::string> items;
    if (!first.empty()) items.push_back(first);
    for (TlsVersion v : kTlsVersions) items.push_back(tls_version_label(v));
    return items;
}

} // namespace

// ===========================================================================
// ROM images

@interface EMRomController : NSObject <NSTableViewDataSource, NSTableViewDelegate>
@end

@implementation EMRomController {
  @public
    const UserDirs* _dirs;
    const Model* _required;
    const ModelVariant* _requiredVariant;
    bool _needAny;
    std::vector<RomStatusRow> _rows;
    NSPanel* _panel;
    NSTableView* _table;
}

- (void)build {
    _panel = make_panel(@"ROM Images", NSMakeSize(680, 380));
    NSString* intro =
        _required && !model_roms_ready(*_required, _dirs->roms(), _requiredVariant)
            ? @"The ROM images the terminal needs are missing or damaged. Import them to continue: choose the "
              @"files, or drop them on the list. Images are recognised by their SHA-256 and stored under the "
              @"names the emulator expects."
        : _needAny && ready_models(_dirs->roms()).empty()
            ? @"No terminal can run yet. Import the ROM images of at least one terminal model to continue "
              @"(choose the files, or drop them on the list); the others can follow later. Images are "
              @"recognised by their SHA-256 and stored under the names the emulator expects."
            : @"The ROM images each terminal model needs, and their state. Import adds images (or drop files "
              @"on the list); they are recognised by their SHA-256.";
    NSTextField* text = wrapping_label(intro, 640);
    _table = [NSTableView new];
    [_table addTableColumn:column(@"model", @"Model", 120)];
    [_table addTableColumn:column(@"file", @"File", 110)];
    [_table addTableColumn:column(@"rom", @"ROM", 210)];
    [_table addTableColumn:column(@"status", @"Status", 190)];
    _table.dataSource = self;
    _table.delegate = self;
    [_table registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    NSScrollView* sv = table_in_scroll(_table, 640, 180);
    NSTextField* folder = small_label(ns("ROM folder: " + _dirs->roms()));
    folder.selectable = YES;
    folder.lineBreakMode = NSLineBreakByTruncatingMiddle;
    [folder.widthAnchor constraintLessThanOrEqualToConstant:480].active = YES;
    NSButton* show = push_button(@"Show in Finder", self, @selector(showFolder:));
    NSButton* import = push_button(@"Import…", self, @selector(import:));
    NSButton* remove = push_button(@"Remove", self, @selector(remove:));
    NSButton* recheck = push_button(@"Check Again", self, @selector(recheck:));
    NSButton* done = push_button(@"Done", self, @selector(done:));
    make_default(done);
    NSStackView* content = vstack(@[
        text, sv, hstack(@[ folder, show ]),
        button_row(@[ import, remove, recheck ], @[ done ])
    ], 10);
    [content.arrangedSubviews[4].widthAnchor constraintEqualToAnchor:content.widthAnchor].active = YES;
    [sv.widthAnchor constraintEqualToAnchor:content.widthAnchor].active = YES;
    set_content(_panel, content);
    [self fill];
}

- (void)fill {
    const NSInteger keep = _table.selectedRow;
    _rows = rom_status(_dirs->roms());
    if (_required) {
        const std::string want = _required->info().name;
        std::stable_sort(_rows.begin(), _rows.end(), [&](const RomStatusRow& a, const RomStatusRow& b) {
            return (a.model == want) > (b.model == want);
        });
    }
    [_table reloadData];
    select_row(_table, std::min<NSInteger>(keep, NSInteger(_rows.size()) - 1));
}

- (NSInteger)numberOfRowsInTableView:(NSTableView*)t {
    (void)t;
    return NSInteger(_rows.size());
}

- (NSView*)tableView:(NSTableView*)t viewForTableColumn:(NSTableColumn*)col row:(NSInteger)row {
    if (row < 0 || size_t(row) >= _rows.size()) return nil;
    const RomStatusRow& r = _rows[size_t(row)];
    std::string text;
    if ([col.identifier isEqualToString:@"model"])
        text = "IBM " + r.model + (r.model_status == "bring-up" ? "" : " [" + r.model_status + "]");
    else if ([col.identifier isEqualToString:@"file"])
        text = r.spec.file;
    else if ([col.identifier isEqualToString:@"rom"])
        text = r.spec.label;
    else {
        text = rom_file_state_name(r.status.state);
        if (!r.status.note.empty()) text += " (" + r.status.note + ")";
    }
    return cell_text(t, col, text);
}

- (void)importFiles:(const std::vector<std::string>&)files {
    std::string report;
    for (const std::string& f : files) {
        std::string stored, note, error;
        if (_dirs->import_rom(f, stored, note, error)) report += stored + ": " + note + "\n";
        else report += error + "\n";
    }
    [self fill];
    if (!report.empty()) info_alert(_panel, "ROM images imported", trim(report));
}

- (void)import:(id)sender {
    (void)sender;
    guarded([&] {
        NSOpenPanel* open = [NSOpenPanel openPanel];
        open.title = @"Import ROM Images";
        open.prompt = @"Import";
        open.allowsMultipleSelection = YES;
        open.canChooseDirectories = NO;
        open.message = @"Choose the ROM images (.bin or .rom files, any name).";
        __block NSModalResponse r = NSModalResponseCancel;
        [open beginSheetModalForWindow:self->_panel
                     completionHandler:^(NSModalResponse resp) {
                       r = resp;
                       [NSApp stopModalWithCode:resp];
                     }];
        [NSApp runModalForWindow:open];
        if (r != NSModalResponseOK) return;
        std::vector<std::string> files;
        for (NSURL* u in open.URLs) files.push_back(str(u.path));
        [self importFiles:files];
    });
}

- (void)remove:(id)sender {
    (void)sender;
    guarded([&] {
        const NSInteger sel = _table.selectedRow;
        if (sel < 0 || size_t(sel) >= _rows.size()) return;
        const RomStatusRow& r = _rows[size_t(sel)];
        if (r.status.state == RomFileStatus::State::Missing) return;
        if (!ask(_panel, "Remove " + r.spec.file + " from the ROM folder?",
                 "The terminal cannot start without it until it is imported again.", "Remove", "Cancel",
                 NSAlertStyleWarning))
            return;
        std::string error;
        if (!_dirs->remove_rom(r.spec.file, error)) error_alert(_panel, "Cannot remove the ROM image", error);
        [self fill];
    });
}

- (void)recheck:(id)sender {
    (void)sender;
    guarded([&] { [self fill]; });
}

- (void)showFolder:(id)sender {
    (void)sender;
    [NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:ns(_dirs->roms())]];
}

- (void)done:(id)sender {
    (void)sender;
    end_modal(NSModalResponseOK);
}

// Files dropped on the list are imported.
- (NSDragOperation)tableView:(NSTableView*)t
                validateDrop:(id<NSDraggingInfo>)info
                 proposedRow:(NSInteger)row
       proposedDropOperation:(NSTableViewDropOperation)op {
    (void)info;
    (void)row;
    (void)op;
    [t setDropRow:-1 dropOperation:NSTableViewDropOn];
    return NSDragOperationCopy;
}

- (BOOL)tableView:(NSTableView*)t
       acceptDrop:(id<NSDraggingInfo>)info
              row:(NSInteger)row
    dropOperation:(NSTableViewDropOperation)op {
    (void)t;
    (void)row;
    (void)op;
    NSArray<NSURL*>* urls = [info.draggingPasteboard readObjectsForClasses:@[ NSURL.class ]
                                                                   options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    std::vector<std::string> files;
    for (NSURL* u in urls) files.push_back(str(u.path));
    if (files.empty()) return NO;
    // After the drop finishes: the import may show an alert.
    dispatch_async(dispatch_get_main_queue(), ^{
      guarded([&] { [self importFiles:files]; });
    });
    return YES;
}

@end

bool em34xx::cocoa::rom_manager_dialog(NSWindow* parent, const UserDirs& dirs, const Model* required,
                                       bool need_any, const ModelVariant* required_variant) {
    EMRomController* c = [EMRomController new];
    c->_dirs = &dirs;
    c->_required = required;
    c->_requiredVariant = required_variant;
    c->_needAny = need_any;
    [c build];
    run_sheet(c->_panel, parent);
    if (required) return model_roms_ready(*required, dirs.roms(), required_variant);
    return !need_any || !ready_models(dirs.roms()).empty();
}

bool em34xx::cocoa::ensure_model_roms(NSWindow* parent, const UserDirs& dirs, const Model& model,
                                      const std::string& what, const ModelVariant* variant) {
    if (model_roms_ready(model, dirs.roms(), variant)) return true;
    const std::string name = "IBM " + (variant ? variant->id : model.info().name);
    const std::string message =
        (what.empty() ? "The " + name + " needs" : what + " is for the " + name + ", which needs") +
        " ROM images that are not in the ROM folder.";
    const std::string detail = unusable_roms_text(model, dirs.roms(), variant) +
                               "\nThe emulator runs the terminal's own firmware and does not include it. "
                               "Import the missing images to use this terminal.";
    if (!ask(parent, message, detail, "Import…", "Not Now", NSAlertStyleWarning)) return false;
    return rom_manager_dialog(parent, dirs, &model, false, variant);
}

// ===========================================================================
// Settings

@interface EMSettingsController : NSObject <NSWindowDelegate>
@end

@implementation EMSettingsController {
  @public
    const UserDirs* _dirs;
    AppSettings* _settings;
    std::function<void(const AppSettings&)> _changed;
    std::vector<std::string> _profiles;
    NSWindow* _window;
    NSButton* _startup;
    NSPopUpButton* _startupProfile;
    NSPopUpButton* _speed;
    NSPopUpButton* _tlsMin;
    NSPopUpButton* _burn;
}

- (void)build {
    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 480, 300)
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                            backing:NSBackingStoreBuffered
                                              defer:YES];
    _window.releasedWhenClosed = NO;
    if (@available(macOS 13.0, *)) _window.title = @"Settings";
    else _window.title = @"Preferences";
    _window.delegate = self;

    _startup = check_box(@"Open a session on startup:", self, @selector(changed:));
    _startupProfile = popup({}, self, @selector(changed:));
    [_startupProfile.widthAnchor constraintGreaterThanOrEqualToConstant:180].active = YES;
    std::vector<std::string> speeds;
    for (unsigned s : kSpeeds) speeds.push_back(s ? format("%u %%", s) : std::string("Unthrottled"));
    _speed = popup(speeds, self, @selector(changed:));
    _tlsMin = popup(tls_version_items(""), self, @selector(changed:));
    _burn = popup({"Off", "Slow", "Fast", "Hyper"}, self, @selector(changed:));
    NSTextField* burnNote = small_label(@"How fast the picture wears the tube. View > Burn-in > Replace CRT fits a new one.");
    NSTextField* speedNote = small_label(@"Of the terminal's own clock; applies at once.");
    NSTextField* tlsNote = small_label(@"For profiles that set none; applies at the next connection.");
    NSButton* roms = push_button(@"ROM Images…", self, @selector(roms:));
    NSTextField* folder = small_label(ns(_dirs->root()));
    folder.selectable = YES;
    folder.lineBreakMode = NSLineBreakByTruncatingMiddle;
    [folder.widthAnchor constraintLessThanOrEqualToConstant:300].active = YES;
    NSButton* show = push_button(@"Show in Finder", self, @selector(showFolder:));

    NSGridView* g = form(@[
        @[ label(@"Startup:"), hstack(@[ _startup, _startupProfile ]) ],
        @[ label(@"Speed:"), _speed ],
        @[ empty(), speedNote ],
        @[ label(@"Minimum TLS version:"), _tlsMin ],
        @[ empty(), tlsNote ],
        @[ label(@"Screen burn-in:"), _burn ],
        @[ empty(), burnNote ],
        @[ label(@"ROM images:"), roms ],
        @[ label(@"User folder:"), vstack(@[ folder, show ], 4) ],
    ]);
    [g rowAtIndex:2].topPadding = -4;
    [g rowAtIndex:4].topPadding = -4;
    [g rowAtIndex:6].topPadding = -4;
    [g rowAtIndex:7].topPadding = 8;
    NSTextField* note = small_label(@"Settings, ROM images, the keymap and the connection profiles are kept in "
                                    @"the user folder.");
    set_content(_window, vstack(@[ g, note ], 14));
}

- (void)load {
    _profiles = _dirs->list_profiles();
    [_startupProfile removeAllItems];
    [_startupProfile.menu addItem:[[NSMenuItem alloc] initWithTitle:@"(none)" action:nil keyEquivalent:@""]];
    NSInteger sel = 0;
    for (size_t i = 0; i < _profiles.size(); ++i) {
        [_startupProfile.menu addItem:[[NSMenuItem alloc] initWithTitle:ns(_profiles[i]) action:nil keyEquivalent:@""]];
        if (_profiles[i] == _settings->startup_profile) sel = NSInteger(i + 1);
    }
    [_startupProfile selectItemAtIndex:sel];
    _startup.state = _settings->startup_open ? NSControlStateValueOn : NSControlStateValueOff;
    _startupProfile.enabled = _settings->startup_open;
    NSInteger speed = 2;
    for (size_t i = 0; i < kSpeedCount; ++i)
        if (kSpeeds[i] == _settings->speed_percent) speed = NSInteger(i);
    [_speed selectItemAtIndex:speed];
    NSInteger tls = 3;
    for (size_t i = 0; i < kTlsVersionCount; ++i)
        if (kTlsVersions[i] == _settings->tls_min_version) tls = NSInteger(i);
    [_tlsMin selectItemAtIndex:tls];
    [_burn selectItemAtIndex:NSInteger(_settings->burn_in)];
}

- (void)changed:(id)sender {
    (void)sender;
    guarded([&] {
        const AppSettings before = *_settings;
        AppSettings& s = *_settings;
        s.startup_open = _startup.state == NSControlStateValueOn;
        _startupProfile.enabled = s.startup_open;
        const NSInteger pr = _startupProfile.indexOfSelectedItem;
        // A profile that no longer exists stays as it was until another is chosen.
        if (pr > 0 && size_t(pr) <= _profiles.size()) s.startup_profile = _profiles[size_t(pr - 1)];
        else if (pr == 0) s.startup_profile.clear();
        const NSInteger sp = _speed.indexOfSelectedItem;
        if (sp >= 0 && size_t(sp) < kSpeedCount) s.speed_percent = kSpeeds[sp];
        const NSInteger tv = _tlsMin.indexOfSelectedItem;
        if (tv >= 0 && size_t(tv) < kTlsVersionCount) s.tls_min_version = kTlsVersions[tv];
        const NSInteger burn = _burn.indexOfSelectedItem;
        if (burn >= 0 && burn <= NSInteger(BurnSpeed::Hyper)) s.burn_in = BurnSpeed(burn);
        if (_changed) _changed(before);
    });
}

- (void)roms:(id)sender {
    (void)sender;
    guarded([&] { rom_manager_dialog(_window, *_dirs, nullptr); });
}

- (void)showFolder:(id)sender {
    (void)sender;
    reveal_in_finder(_dirs->root());
}

@end

namespace {
EMSettingsController* g_settings = nil;
}

void em34xx::cocoa::show_settings_window(const UserDirs& dirs, AppSettings& settings,
                                         std::function<void(const AppSettings&)> changed) {
    if (!g_settings) {
        g_settings = [EMSettingsController new];
        g_settings->_dirs = &dirs;
        g_settings->_settings = &settings;
        g_settings->_changed = std::move(changed);
        [g_settings build];
        [g_settings load];
        [g_settings->_window center];
    } else {
        [g_settings load];
    }
    [g_settings->_window makeKeyAndOrderFront:nil];
}

void em34xx::cocoa::refresh_settings_window() {
    if (g_settings && g_settings->_window.visible) [g_settings load];
}

// ===========================================================================
// Name prompt

bool em34xx::cocoa::name_dialog(NSWindow* parent, const std::string& title, const std::string& prompt,
                                const std::string& ok, std::string& name) {
    std::string value = name;
    for (;;) {
        NSAlert* a = [NSAlert new];
        a.messageText = ns(title);
        a.informativeText = ns(prompt);
        [a addButtonWithTitle:ns(ok)];
        [a addButtonWithTitle:@"Cancel"];
        NSTextField* field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 260, 22)];
        field.stringValue = ns(value);
        a.accessoryView = field;
        a.window.initialFirstResponder = field;
        [a layout];
        if (run_alert(a, parent) != NSAlertFirstButtonReturn) return false;
        value = trim(str(field.stringValue));
        std::string why;
        if (UserDirs::valid_profile_name(value, why)) break;
        error_alert(parent, "That name cannot be used.", why);
    }
    name = value;
    return true;
}

// ===========================================================================
// TLS settings

namespace {
// A certificate fetch runs on its own thread; the sheet may close first.
struct FetchJob {
    std::mutex mu;
    bool wanted = true;   // false once the sheet is gone or a newer fetch started
};

// The session whose certificate Fetch reads: the first that uses TLS
// (session 1 when none does yet).
size_t tls_fetch_session(const Profile& p) {
    for (size_t i = 0; i < Profile::kSessions; ++i)
        if (p.sessions[i].used() && p.sessions[i].tls) return i;
    return 0;
}
} // namespace

@interface EMTlsController : NSObject
@end

@implementation EMTlsController {
  @public
    Profile _p;
    const AppSettings* _s;
    std::shared_ptr<FetchJob> _job;
    NSPanel* _panel;
    NSButton* _system;
    NSButton* _any;
    NSButton* _hash;
    NSTextField* _hashField;
    NSButton* _fetch;
    NSTextView* _cert;
    NSPopUpButton* _minVer;
}

- (void)build {
    _panel = make_panel(@"TLS Settings", NSMakeSize(560, 420));
    _system = radio(@"Verify the certificate normally (system trust store and host name)", self, @selector(mode:));
    _any = radio(@"Accept all certificates (insecure: anyone can intercept the connection)", self, @selector(mode:));
    _hash = radio(@"Accept only the certificate with this SHA-256 fingerprint:", self, @selector(mode:));
    _hashField = text_field(ns(_p.tls_cert_sha256), 520);
    _hashField.placeholderString = @"AB:CD:… (32 bytes in hex; colons optional)";
    _hashField.font = [NSFont monospacedSystemFontOfSize:NSFont.smallSystemFontSize weight:NSFontWeightRegular];
    _fetch = push_button(@"Fetch", self, @selector(fetch:));
    const size_t fs = tls_fetch_session(_p);
    const ProfileSession& src = _p.sessions[fs];
    NSTextField* from = small_label(src.used() ? ns(format("Reads the certificate from %s:%u (session %zu).",
                                                           src.host.c_str(), unsigned(src.port), fs + 1))
                                               : @"Enter the host of session 1 first.");
    _fetch.enabled = src.used();
    NSTextView* cert = nil;
    NSScrollView* certView = text_view(520, 130, &cert);
    _cert = cert;
    _minVer = popup(tls_version_items("Default (" + tls_version_label(_s->tls_min_version) + ")"), nil, nil);
    [_minVer selectItemAtIndex:0];
    for (size_t i = 0; i < kTlsVersionCount; ++i) {
        TlsVersion v;
        if (!_p.tls_min_version.empty() && parse_tls_version(_p.tls_min_version, v) && v == kTlsVersions[i])
            [_minVer selectItemAtIndex:NSInteger(i + 1)];
    }
    NSButton* cancel = push_button(@"Cancel", self, @selector(cancel:));
    make_cancel(cancel);
    NSButton* ok = push_button(@"OK", self, @selector(ok:));
    make_default(ok);
    NSStackView* hashRow = vstack(@[ _hashField, hstack(@[ _fetch, from ]) ], 6);
    hashRow.edgeInsets = NSEdgeInsetsMake(0, 18, 0, 0);
    NSStackView* content = vstack(@[
        label(@"Certificate check for the sessions that use TLS:"), _system, _any, _hash, hashRow, certView,
        hstack(@[ label(@"Minimum TLS version:"), _minVer ]), button_row(@[], @[ cancel, ok ])
    ], 8);
    [content.arrangedSubviews.lastObject.widthAnchor constraintEqualToAnchor:content.widthAnchor].active = YES;
    set_content(_panel, content);
    NSButton* on = _p.tls_mode == ProfileTlsMode::System ? _system : _p.tls_mode == ProfileTlsMode::AcceptAll ? _any : _hash;
    on.state = NSControlStateValueOn;
    [self mode:nil];
}

- (void)mode:(id)sender {
    (void)sender;
    _hashField.enabled = _hash.state == NSControlStateValueOn;
}

- (void)fetch:(id)sender {
    (void)sender;
    guarded([&] {
        if (_job) {
            std::lock_guard<std::mutex> g(_job->mu);
            _job->wanted = false;   // forget an earlier fetch still running
        }
        auto job = std::make_shared<FetchJob>();
        _job = job;
        Tn5250Config cfg;
        const ProfileSession& src = _p.sessions[tls_fetch_session(_p)];
        cfg.host = src.host;
        cfg.port = src.port;
        cfg.tls = true;
        cfg.tls_min_version = _s->tls_min_version;
        const NSInteger mv = _minVer.indexOfSelectedItem;
        if (mv > 0) cfg.tls_min_version = kTlsVersions[mv - 1];
        _fetch.enabled = NO;
        set_text(_cert, format("Connecting to %s:%u…", cfg.host.c_str(), unsigned(cfg.port)));
        __weak EMTlsController* weak = self;
        std::thread([job, cfg, weak] {
            auto peer = std::make_shared<tn5250::PeerCertificate>();
            auto error = std::make_shared<std::string>();
            const bool ok = tn5250::tls_fetch_certificate(cfg, *peer, *error);
            dispatch_async(dispatch_get_main_queue(), ^{
              EMTlsController* c = weak;
              {
                  std::lock_guard<std::mutex> g(job->mu);
                  if (!job->wanted || !c) return;
              }
              guarded([&] { [c fetched:ok peer:*peer error:*error]; });
            });
        }).detach();
    });
}

- (void)fetched:(bool)ok peer:(const tn5250::PeerCertificate&)peer error:(const std::string&)error {
    _fetch.enabled = YES;
    if (!ok) {
        set_text(_cert, "Could not fetch the certificate:\n" + error);
        return;
    }
    set_text(_cert, certificate_text(peer));
    if (peer.have_digest) {
        _hashField.stringValue = ns(format_sha256_fingerprint(peer.sha256));
        _hash.state = NSControlStateValueOn;
        [self mode:nil];
    }
}

- (void)forget {
    if (!_job) return;
    std::lock_guard<std::mutex> g(_job->mu);
    _job->wanted = false;
}

- (void)ok:(id)sender {
    (void)sender;
    guarded([&] {
        Profile p = _p;
        p.tls_mode = _any.state == NSControlStateValueOn    ? ProfileTlsMode::AcceptAll
                     : _hash.state == NSControlStateValueOn ? ProfileTlsMode::Fingerprint
                                                            : ProfileTlsMode::System;
        p.tls_cert_sha256 = trim(str(_hashField.stringValue));
        Sha256Digest digest;
        if (!p.tls_cert_sha256.empty() && parse_sha256_fingerprint(p.tls_cert_sha256, digest))
            p.tls_cert_sha256 = format_sha256_fingerprint(digest);   // canonical form
        if (p.tls_mode == ProfileTlsMode::Fingerprint && !parse_sha256_fingerprint(p.tls_cert_sha256, digest)) {
            error_alert(_panel, "The fingerprint is not a SHA-256 fingerprint.",
                        "It takes 32 bytes in hex, colons optional. Use Fetch to read it from the server.");
            return;
        }
        const NSInteger mv = _minVer.indexOfSelectedItem;
        p.tls_min_version = mv > 0 ? tls_version_name(kTlsVersions[mv - 1]) : "";
        _p = p;
        end_modal(NSModalResponseOK);
    });
}

- (void)cancel:(id)sender {
    (void)sender;
    end_modal(NSModalResponseCancel);
}

@end

bool em34xx::cocoa::tls_settings_dialog(NSWindow* parent, Profile& p, const AppSettings& s) {
    EMTlsController* c = [EMTlsController new];
    c->_p = p;
    c->_s = &s;
    [c build];
    const NSModalResponse r = run_sheet(c->_panel, parent);
    [c forget];
    if (r != NSModalResponseOK) return false;
    p = c->_p;
    return true;
}

// ===========================================================================
// Profile editor


namespace {
std::string tls_summary(const Profile& p, const AppSettings& s) {
    std::string m;
    switch (p.tls_mode) {
    case ProfileTlsMode::System: m = "verify normally"; break;
    case ProfileTlsMode::AcceptAll: m = "accept all certificates (insecure)"; break;
    case ProfileTlsMode::Fingerprint: m = "accept the certificate " + p.tls_cert_sha256.substr(0, 23) + "…"; break;
    }
    TlsVersion v = s.tls_min_version;
    if (!p.tls_min_version.empty()) parse_tls_version(p.tls_min_version, v);
    return m + ", minimum " + tls_version_label(v);
}
} // namespace

@interface EMProfileController : NSObject <NSTextFieldDelegate>
@end

@implementation EMProfileController {
  @public
    Profile _p;
    ProfileEditMode _mode;
    const AppSettings* _s;
    const UserDirs* _dirs;
    std::string _title;
    // The terminal type list, in order: a variant of a model per entry.
    std::vector<const Model*> _models;
    std::vector<const ModelVariant*> _variants;
    NSInteger _modelSel;                 // the selection to go back to
    NSPanel* _panel;
    NSPopUpButton* _model;
    NSPopUpButton* _subtype;
    NSButton* _crt;
    NSTextField* _host[2];
    NSTextField* _port[2];
    NSTextField* _dev[2];
    NSButton* _tls[2];
    NSTextField* _tlsSummary;
    NSButton* _tlsButton;
}

- (NSView*)sessionBox:(size_t)i {
    _host[i] = text_field(nil, 220);
    _host[i].placeholderString = i == 0 ? @"host name or address" : @"optional";
    _port[i] = text_field(nil, 60);
    _dev[i] = text_field(nil, 110);
    _dev[i].placeholderString = @"optional";
    _dev[i].delegate = self;
    _tls[i] = check_box(@"Use TLS", self, @selector(tlsToggled:));
    _tls[i].tag = NSInteger(i);
    NSGridView* g = form(@[
        @[ label(@"Host:"), hstack(@[ _host[i], label(@"Port:"), _port[i] ]) ],
        @[ label(@"Device name:"), hstack(@[ _dev[i], _tls[i] ], 16) ],
    ]);
    return group(i == 0 ? @"Session 1" : @"Session 2", g);
}

// The terminal type list: every variant of every model that can be
// emulated. One whose model lacks its ROM images is disabled (grey) and
// says so; "Import ROM Images…" below them opens the ROM dialog. A profile
// that is for such a unit keeps it selected.
- (void)fillModels:(const std::string&)select {
    [_model removeAllItems];
    _model.autoenablesItems = NO;
    _models.clear();
    _variants.clear();
    NSInteger sel = 0;
    bool lacking = false;
    for (const Model* m : runnable_models()) {
        for (const ModelVariant& v : m->info().variants) {
            const bool ready = model_roms_ready(*m, _dirs->roms(), &v);
            NSMenuItem* item = [[NSMenuItem alloc]
                initWithTitle:ns("IBM " + v.id + " (" + v.description + (ready ? ")" : ", ROM images needed)"))
                       action:nil
                keyEquivalent:@""];
            item.enabled = ready;
            if (!ready) {
                item.toolTip = ns("Needs " + trim(unusable_roms_text(*m, _dirs->roms(), &v)));
                lacking = true;
            }
            [_model.menu addItem:item];
            if (v.id == select) sel = NSInteger(_variants.size());
            _models.push_back(m);
            _variants.push_back(&v);
        }
    }
    if (lacking) {
        [_model.menu addItem:[NSMenuItem separatorItem]];
        [_model.menu addItem:[[NSMenuItem alloc] initWithTitle:@"Import ROM Images…" action:nil keyEquivalent:@""]];
    }
    [_model selectItemAtIndex:sel];
    _modelSel = sel;
}

// The CRT type list: the tubes the selected variant can be shown with. A
// colour unit has one tube, and the list says so.
- (void)fillPhosphors:(const std::string&)select {
    [_subtype removeAllItems];
    const NSInteger sel = _model.indexOfSelectedItem;
    const ModelVariant* v = sel >= 0 && size_t(sel) < _variants.size() ? _variants[size_t(sel)] : nullptr;
    if (!v || v->phosphors.empty()) {
        [_subtype addItemWithTitle:v ? @"Colour" : @""];
        _subtype.enabled = NO;
        return;
    }
    NSInteger at = 0;
    for (size_t k = 0; k < v->phosphors.size(); ++k) {
        [_subtype addItemWithTitle:ns(v->phosphors[k].label)];
        if (v->phosphors[k].id == select) at = NSInteger(k);
    }
    [_subtype selectItemAtIndex:at];
    _subtype.enabled = YES;
}

// The phosphor selected in the CRT type list ("" for a colour unit).
- (std::string)selectedPhosphor {
    const NSInteger sel = _model.indexOfSelectedItem;
    if (sel < 0 || size_t(sel) >= _variants.size()) return {};
    const std::vector<Phosphor>& tubes = _variants[size_t(sel)]->phosphors;
    const NSInteger st = _subtype.indexOfSelectedItem;
    return st >= 0 && size_t(st) < tubes.size() ? tubes[size_t(st)].id : std::string();
}

- (void)modelChosen:(id)sender {
    (void)sender;
    guarded([&] {
        // The tube carries over when the new variant has it too.
        const std::string tube = [self selectedPhosphor].empty() ? _p.phosphor : [self selectedPhosphor];
        const NSInteger sel = _model.indexOfSelectedItem;
        if (sel >= 0 && size_t(sel) < _variants.size()) {
            _modelSel = sel;
            [self fillPhosphors:tube];
            return;
        }
        // "Import ROM Images…": the ROM dialog, then the list again (an
        // import can ready several models) with the previous choice.
        const std::string back =
            _modelSel >= 0 && size_t(_modelSel) < _variants.size() ? _variants[size_t(_modelSel)]->id : _p.variant;
        rom_manager_dialog(_panel, *_dirs, nullptr);
        [self fillModels:back];
        [self fillPhosphors:tube];
    });
}

- (void)phosphorChosen:(id)sender {
    (void)sender;
    guarded([&] { _p.phosphor = [self selectedPhosphor]; });
}

- (void)build {
    _panel = make_panel(ns(_title), NSMakeSize(520, 440));
    _model = popup({}, self, @selector(modelChosen:));
    _subtype = popup({}, self, @selector(phosphorChosen:));
    _crt = check_box(@"Scan lines", nil, nil);
    NSGridView* terminal = form(@[
        @[ label(@"Terminal type:"), _model ],
        @[ label(@"CRT type:"), hstack(@[ _subtype, _crt ], 16) ],
    ]);
    NSView* s1 = [self sessionBox:0];
    NSView* s2 = [self sessionBox:1];
    _tlsSummary = wrapping_label(@"", 340);
    _tlsSummary.selectable = NO;
    _tlsButton = push_button(@"TLS Settings…", self, @selector(tlsSettings:));
    NSStackView* tlsRow = hstack(@[ _tlsSummary, _tlsButton ], 12);
    tlsRow.alignment = NSLayoutAttributeCenterY;
    NSButton* cancel = push_button(@"Cancel", self, @selector(cancel:));
    make_cancel(cancel);
    NSButton* ok = push_button(_mode == ProfileEditMode::Connect ? @"Connect"
                               : _mode == ProfileEditMode::Save  ? @"Save"
                                                                 : @"OK",
                               self, @selector(ok:));
    make_default(ok);
    NSStackView* content = vstack(@[ terminal, s1, s2, tlsRow, button_row(@[], @[ cancel, ok ]) ], 14);
    for (NSView* v in @[ s1, s2, content.arrangedSubviews.lastObject ])
        [v.widthAnchor constraintEqualToAnchor:content.widthAnchor].active = YES;
    set_content(_panel, content);

    _p.normalize();
    [self fillModels:_p.variant];
    [self fillPhosphors:_p.phosphor];
    _crt.state = _p.crt ? NSControlStateValueOn : NSControlStateValueOff;
    for (size_t s = 0; s < 2; ++s) {
        const ProfileSession& ps = _p.sessions[s];
        _host[s].stringValue = ns(ps.host);
        _port[s].stringValue = ps.used() || s == 0 ? ns(std::to_string(ps.port)) : @"";
        _dev[s].stringValue = ns(ps.device_name);
        _tls[s].state = ps.tls ? NSControlStateValueOn : NSControlStateValueOff;
    }
    _panel.initialFirstResponder = _host[0];
    [self updateTls];
}

// Device names have at most 10 characters (Windows' edit limit, likewise).
- (void)controlTextDidChange:(NSNotification*)n {
    NSTextField* f = n.object;
    if (f.stringValue.length > 10) f.stringValue = [f.stringValue substringToIndex:10];
}

// Read the fields into _p; false (with a message) when a port is invalid.
- (bool)read:(bool)quiet {
    const NSInteger m = _model.indexOfSelectedItem;
    if (m >= 0 && size_t(m) < _variants.size()) {
        _p.model = _models[size_t(m)]->info().name;
        _p.variant = _variants[size_t(m)]->id;
        _p.phosphor = [self selectedPhosphor];
    }
    _p.crt = _crt.state == NSControlStateValueOn;
    for (size_t i = 0; i < 2; ++i) {
        ProfileSession& s = _p.sessions[i];
        s.host = trim(str(_host[i].stringValue));
        s.device_name = trim(str(_dev[i].stringValue));
        s.tls = _tls[i].state == NSControlStateValueOn;
        const std::string port = trim(str(_port[i].stringValue));
        uint64_t v = 0;
        if (port.empty()) v = s.tls ? 992 : 23;
        else if (!parse_uint(port, v) || v == 0 || v > 65535) {
            if (!quiet)
                error_alert(_panel, format("The port of session %zu is not valid.", i + 1),
                            "A port is a number from 1 to 65535.");
            return false;
        }
        s.port = uint16_t(v);
    }
    return true;
}

- (void)updateTls {
    std::string which;
    for (size_t i = 0; i < 2; ++i)
        if (_tls[i].state == NSControlStateValueOn)
            which += which.empty() ? format("session %zu", i + 1) : format(" and %zu", i + 1);
    _tlsButton.enabled = !which.empty();
    _tlsSummary.stringValue = which.empty() ? @"No session uses TLS: plain telnet."
                                            : ns("TLS for " + which + ": " + tls_summary(_p, *_s) + ".");
}

// Switch the session's conventional port along with TLS.
- (void)tlsToggled:(NSButton*)sender {
    const size_t i = sender.tag == 0 ? 0 : 1;
    const bool on = sender.state == NSControlStateValueOn;
    const std::string port = trim(str(_port[i].stringValue));
    if (port == (on ? "23" : "992")) _port[i].stringValue = on ? @"992" : @"23";
    [self updateTls];
}

- (void)tlsSettings:(id)sender {
    (void)sender;
    guarded([&] {
        [self read:true];
        if (tls_settings_dialog(_panel, _p, *_s)) [self updateTls];
    });
}

- (void)ok:(id)sender {
    (void)sender;
    guarded([&] {
        if (![self read:false]) return;
        const std::string why = _p.validate();
        if (!why.empty()) {
            error_alert(_panel, "The profile is not complete.", why);
            return;
        }
        end_modal(NSModalResponseOK);
    });
}

- (void)cancel:(id)sender {
    (void)sender;
    end_modal(NSModalResponseCancel);
}

@end

bool em34xx::cocoa::profile_editor_dialog(NSWindow* parent, Profile& p, ProfileEditMode mode, const AppSettings& s,
                                          const UserDirs& dirs, const std::string& title) {
    EMProfileController* c = [EMProfileController new];
    c->_p = p;
    c->_mode = mode;
    c->_s = &s;
    c->_dirs = &dirs;
    c->_modelSel = 0;
    c->_title = title;
    [c build];
    if (run_sheet(c->_panel, parent) != NSModalResponseOK) return false;
    p = c->_p;
    return true;
}

// ===========================================================================
// Profile manager

@interface EMManagerController : NSObject <NSTableViewDataSource, NSTableViewDelegate, NSMenuDelegate>
@end

@implementation EMManagerController {
  @public
    const UserDirs* _dirs;
    const AppSettings* _s;
    std::vector<std::string> _names;
    std::string _chosen;
    NSPanel* _panel;
    NSTableView* _table;
    NSButton* _connect;
    NSSegmentedControl* _addRemove;
}

- (NSMenu*)actionMenu {
    NSMenu* m = [NSMenu new];
    m.delegate = self;
    [m addItemWithTitle:@"Edit…" action:@selector(edit:) keyEquivalent:@""].target = self;
    [m addItemWithTitle:@"Rename…" action:@selector(rename:) keyEquivalent:@""].target = self;
    [m addItemWithTitle:@"Duplicate" action:@selector(duplicate:) keyEquivalent:@""].target = self;
    [m addItem:NSMenuItem.separatorItem];
    [m addItemWithTitle:@"Move to Trash" action:@selector(trash:) keyEquivalent:@""].target = self;
    [m addItem:NSMenuItem.separatorItem];
    [m addItemWithTitle:@"Show in Finder" action:@selector(reveal:) keyEquivalent:@""].target = self;
    return m;
}

- (void)build {
    _panel = make_panel(@"Connect to Profile", NSMakeSize(420, 360));
    NSTextField* intro = wrapping_label(@"Choose the connection profile to open. Connecting restarts the terminal, "
                                        @"which then signs on to the profile's systems.",
                                        380);
    _table = [NSTableView new];
    NSTableColumn* col = column(@"name", @"Profile", 360);
    [_table addTableColumn:col];
    _table.headerView = nil;
    _table.dataSource = self;
    _table.delegate = self;
    _table.target = self;
    _table.doubleAction = @selector(connect:);
    _table.menu = [self actionMenu];
    NSScrollView* sv = table_in_scroll(_table, 380, 200);

    _addRemove = [NSSegmentedControl segmentedControlWithImages:@[
        [NSImage imageNamed:NSImageNameAddTemplate], [NSImage imageNamed:NSImageNameRemoveTemplate]
    ]
                                                   trackingMode:NSSegmentSwitchTrackingMomentary
                                                         target:self
                                                         action:@selector(addRemove:)];
    _addRemove.translatesAutoresizingMaskIntoConstraints = NO;
    [_addRemove setToolTip:@"New profile" forSegment:0];
    [_addRemove setToolTip:@"Move the profile to the Trash" forSegment:1];
    NSPopUpButton* actions = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:YES];
    actions.translatesAutoresizingMaskIntoConstraints = NO;
    actions.bezelStyle = NSBezelStyleTexturedRounded;
    NSMenu* am = [self actionMenu];
    NSMenuItem* gear = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
    gear.image = [NSImage imageNamed:NSImageNameActionTemplate];
    [am insertItem:gear atIndex:0];
    actions.menu = am;
    actions.toolTip = @"Edit, rename, duplicate or remove the profile";

    NSButton* cancel = push_button(@"Cancel", self, @selector(cancel:));
    make_cancel(cancel);
    _connect = push_button(@"Connect", self, @selector(connect:));
    make_default(_connect);
    NSStackView* content = vstack(@[ intro, sv, button_row(@[ _addRemove, actions ], @[ cancel, _connect ]) ], 12);
    [content.arrangedSubviews.lastObject.widthAnchor constraintEqualToAnchor:content.widthAnchor].active = YES;
    [sv.widthAnchor constraintEqualToAnchor:content.widthAnchor].active = YES;
    set_content(_panel, content);
    _panel.initialFirstResponder = _table;
    [self fill:""];
}

- (void)fill:(const std::string&)select {
    _names = _dirs->list_profiles();
    [_table reloadData];
    NSInteger sel = _names.empty() ? -1 : 0;
    for (size_t i = 0; i < _names.size(); ++i)
        if (_names[i] == select) sel = NSInteger(i);
    select_row(_table, sel);
    [self updateButtons];
    refresh_settings_window();
}

- (void)updateButtons {
    const bool any = _table.selectedRow >= 0;
    _connect.enabled = any;
    [_addRemove setEnabled:any forSegment:1];
}

- (std::string)selected {
    const NSInteger i = _table.clickedRow >= 0 ? _table.clickedRow : _table.selectedRow;
    return i >= 0 && size_t(i) < _names.size() ? _names[size_t(i)] : std::string();
}

- (BOOL)validateMenuItem:(NSMenuItem*)item {
    return item.action == nil || ![self selected].empty();
}

- (NSInteger)numberOfRowsInTableView:(NSTableView*)t {
    (void)t;
    return NSInteger(_names.size());
}

- (NSView*)tableView:(NSTableView*)t viewForTableColumn:(NSTableColumn*)col row:(NSInteger)row {
    return row >= 0 && size_t(row) < _names.size() ? cell_text(t, col, _names[size_t(row)]) : nil;
}

- (void)tableViewSelectionDidChange:(NSNotification*)n {
    (void)n;
    [self updateButtons];
}

- (void)connect:(id)sender {
    (void)sender;
    const std::string sel = [self selected];
    if (sel.empty()) return;
    _chosen = sel;
    end_modal(NSModalResponseOK);
}

- (void)cancel:(id)sender {
    (void)sender;
    end_modal(NSModalResponseCancel);
}

- (void)addRemove:(NSSegmentedControl*)sender {
    if (sender.selectedSegment == 0) [self add:sender];
    else [self trash:sender];
}

- (void)add:(id)sender {
    (void)sender;
    guarded([&] {
        std::string name = _dirs->unique_profile_name("New Profile");
        if (!name_dialog(_panel, "New Profile", "Name of the new profile:", "Continue", name)) return;
        if (_dirs->profile_exists(name)) {
            error_alert(_panel, "A profile named \"" + name + "\" already exists.");
            return;
        }
        Profile p;
        // A new profile is for a terminal that can run.
        const StartModel start = choose_start_model(p.model, _dirs->roms());
        if (start.model) {
            p.model = start.model->info().name;
            if (start.variant) p.variant = start.variant->id;
        }
        if (!profile_editor_dialog(_panel, p, ProfileEditMode::Save, *_s, *_dirs, "New Profile: " + name)) return;
        std::string error;
        if (!save_profile(_dirs->profile_path(name), p, error)) error_alert(_panel, "Cannot save the profile", error);
        [self fill:name];
    });
}

- (void)edit:(id)sender {
    (void)sender;
    guarded([&] {
        const std::string sel = [self selected];
        if (sel.empty()) return;
        Profile p;
        std::string error;
        if (!load_profile(_dirs->profile_path(sel), p, error)) {
            error_alert(_panel, "Cannot open the profile", error);
            return;
        }
        if (!profile_editor_dialog(_panel, p, ProfileEditMode::Save, *_s, *_dirs, "Profile: " + sel)) return;
        if (!save_profile(_dirs->profile_path(sel), p, error)) error_alert(_panel, "Cannot save the profile", error);
    });
}

- (void)rename:(id)sender {
    (void)sender;
    guarded([&] {
        const std::string sel = [self selected];
        if (sel.empty()) return;
        std::string name = sel;
        if (!name_dialog(_panel, "Rename Profile", "New name of \"" + sel + "\":", "Rename", name) || name == sel)
            return;
        std::string error;
        if (!_dirs->rename_profile(sel, name, error)) error_alert(_panel, "Cannot rename the profile", error);
        [self fill:name];
    });
}

- (void)duplicate:(id)sender {
    (void)sender;
    guarded([&] {
        const std::string sel = [self selected];
        if (sel.empty()) return;
        const std::string name = _dirs->unique_profile_name(sel + " copy");
        std::string error;
        if (!_dirs->copy_profile(sel, name, error)) error_alert(_panel, "Cannot duplicate the profile", error);
        [self fill:name];
    });
}

// Into the Trash, where it can be put back: no question asked (Finder's
// convention), unlike Windows' permanent Delete.
- (void)trash:(id)sender {
    (void)sender;
    guarded([&] {
        const std::string sel = [self selected];
        if (sel.empty()) return;
        NSError* err = nil;
        if (![NSFileManager.defaultManager trashItemAtURL:[NSURL fileURLWithPath:ns(_dirs->profile_path(sel))]
                                         resultingItemURL:nil
                                                    error:&err])
            error_alert(_panel, "Cannot move the profile to the Trash", str(err.localizedDescription));
        [self fill:""];
    });
}

- (void)reveal:(id)sender {
    (void)sender;
    const std::string sel = [self selected];
    reveal_in_finder(sel.empty() ? _dirs->profiles() : _dirs->profile_path(sel));
}

@end

std::string em34xx::cocoa::profile_manager_dialog(NSWindow* parent, const UserDirs& dirs, const AppSettings& s) {
    EMManagerController* c = [EMManagerController new];
    c->_dirs = &dirs;
    c->_s = &s;
    [c build];
    if (run_sheet(c->_panel, parent) != NSModalResponseOK) return std::string();
    return c->_chosen;
}

// ===========================================================================
// Trust prompts

namespace {
NSScrollView* details_view(const std::string& text, CGFloat height) {
    NSTextView* tv = nil;
    NSScrollView* sv = text_view(460, height, &tv);
    set_text(tv, text);
    return sv;
}
} // namespace

tn5250::TlsTrustDecision em34xx::cocoa::trust_dialog(NSWindow* parent, const tn5250::TlsTrustRequest& r,
                                                      const std::string& profile) {
    NSAlert* a = [NSAlert new];
    a.alertStyle = NSAlertStyleWarning;
    a.messageText = ns(format("The certificate of %s:%u (session %u) is not trusted", r.host.c_str(),
                              unsigned(r.port), r.station + 1));
    a.informativeText =
        r.problem == tn5250::TlsTrustProblem::PinMismatch
            ? @"It is not the certificate this profile trusts. The server may have a new certificate, or someone "
              @"may be intercepting the connection."
            : @"Someone may be intercepting the connection; connect only if you know this certificate.";
    std::string details = field("Reason", r.reason) + "\n" + certificate_text(r.peer);
    details += "\nTrust Permanently stores the SHA-256 in the profile \"" + profile +
               "\"; only this certificate is accepted afterwards.";
    NSScrollView* sv = details_view(details, 170);
    sv.frame = NSMakeRect(0, 0, 460, 170);
    a.accessoryView = sv;
    [a addButtonWithTitle:@"Stop"];
    [a addButtonWithTitle:@"Connect"].keyEquivalent = @"";
    [a addButtonWithTitle:@"Trust Permanently"].keyEquivalent = @"";
    NSBeep();
    const NSModalResponse resp = run_alert(a, parent);
    if (resp == NSAlertSecondButtonReturn) return tn5250::TlsTrustDecision::AcceptOnce;
    if (resp == NSAlertThirdButtonReturn) return tn5250::TlsTrustDecision::AcceptPermanently;
    return tn5250::TlsTrustDecision::Reject;
}

@interface EMRiskToggle : NSObject
@property(weak) NSButton* connect;
- (void)toggled:(NSButton*)sender;
@end

@implementation EMRiskToggle
- (void)toggled:(NSButton*)sender {
    self.connect.enabled = sender.state == NSControlStateValueOn;
}
@end

bool em34xx::cocoa::certificate_invalid_dialog(NSWindow* parent, const tn5250::TlsTrustRequest& r) {
    NSAlert* a = [NSAlert new];
    a.alertStyle = NSAlertStyleCritical;
    a.messageText = ns(format("The certificate of %s:%u (session %u) is not valid", r.host.c_str(),
                              unsigned(r.port), r.station + 1));
    a.informativeText = @"The connection may not be secure: the server may be badly maintained, this Mac's clock "
                        @"may be wrong, or someone may be intercepting the connection. This cannot be remembered: "
                        @"you are asked again for every connection.";
    std::string errors;
    for (const std::string& e : r.errors) errors += e + "\n";
    if (errors.empty()) errors = r.reason + "\n";
    EMRiskToggle* toggle = [EMRiskToggle new];
    NSButton* understand = check_box(@"I understand the risk and want to connect to this server anyway", toggle,
                                     @selector(toggled:));
    NSStackView* acc = vstack(@[
        label(@"What is wrong with the certificate:"), details_view(errors, 70),
        label(@"The certificate the server sent:"), details_view(certificate_text(r.peer), 120), understand
    ], 6);
    acc.frame = NSMakeRect(0, 0, 460, acc.fittingSize.height);
    a.accessoryView = acc;
    [a addButtonWithTitle:@"Cancel"];
    NSButton* connect = [a addButtonWithTitle:@"Connect Anyway"];
    connect.keyEquivalent = @"";
    connect.enabled = NO;
    toggle.connect = connect;
    NSBeep();
    const NSModalResponse resp = run_alert(a, parent);
    (void)toggle;
    // Only with the box ticked.
    return resp == NSAlertSecondButtonReturn && understand.state == NSControlStateValueOn;
}

// ===========================================================================
// Keymap editor

namespace {
void mods_flags(const std::string& mods, bool& shift, bool& ctrl, bool& alt, bool& any) {
    shift = ctrl = alt = any = false;
    for (const std::string& w : split_words(to_lower(mods))) {
        if (w == "*" || w == "any") any = true;
        else if (w.find("shift") != std::string::npos) shift = true;
        else if (w.find("ctrl") != std::string::npos || w == "control") ctrl = true;
        else if (w.find("alt") != std::string::npos) alt = true;
    }
}

const char* const kKeyboards[] = {"Both", "122-key", "Enhanced"};
const char* const kKeyboardValues[] = {"", "122", "enhanced"};
} // namespace

// The key capture box: click it, then press the key (with its modifiers).
@interface EMKeyCapture : NSView
@property(copy) void (^captured)(uint32_t hid, bool shift, bool ctrl, bool alt);
@property(copy) NSString* text;
@end

@implementation EMKeyCapture
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (BOOL)becomeFirstResponder {
    self.needsDisplay = YES;
    return YES;
}
- (BOOL)resignFirstResponder {
    self.needsDisplay = YES;
    return YES;
}
- (NSSize)intrinsicContentSize {
    return NSMakeSize(260, 26);
}
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    const bool focused = self.window.firstResponder == self;
    NSBezierPath* p = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(self.bounds, 1, 1) xRadius:5 yRadius:5];
    [NSColor.textBackgroundColor setFill];
    [p fill];
    [(focused ? NSColor.keyboardFocusIndicatorColor : NSColor.separatorColor) setStroke];
    p.lineWidth = focused ? 2 : 1;
    [p stroke];
    NSString* t = self.text.length ? self.text : (focused ? @"Press a key…" : @"Click here, then press a key");
    NSDictionary* attrs = @{
        NSFontAttributeName : [NSFont systemFontOfSize:NSFont.systemFontSize],
        NSForegroundColorAttributeName : self.text.length ? NSColor.labelColor : NSColor.placeholderTextColor
    };
    const NSSize sz = [t sizeWithAttributes:attrs];
    [t drawAtPoint:NSMakePoint(8, (self.bounds.size.height - sz.height) / 2) withAttributes:attrs];
}
- (void)mouseDown:(NSEvent*)e {
    (void)e;
    [self.window makeFirstResponder:self];
}
- (void)report:(uint32_t)hid flags:(NSEventModifierFlags)flags {
    if (!hid || !self.captured) return;
    const uint8_t own = host_modifier_bit(hid);
    // A modifier pressed on its own is the key itself, without modifiers.
    const bool shift = (flags & NSEventModifierFlagShift) && !(own & (kModLShift | kModRShift));
    const bool ctrl = (flags & NSEventModifierFlagControl) && !(own & (kModLCtrl | kModRCtrl));
    const bool alt = (flags & NSEventModifierFlagOption) && !(own & (kModLAlt | kModRAlt));
    self.captured(hid, shift, ctrl, alt);
    self.needsDisplay = YES;
}
- (void)keyDown:(NSEvent*)e {
    if (e.modifierFlags & NSEventModifierFlagCommand) return;   // Command belongs to the Mac
    [self report:em34xx::cocoa::hid_from_keycode(e.keyCode) flags:e.modifierFlags];
}
- (void)keyUp:(NSEvent*)e {
    (void)e;
}
- (void)flagsChanged:(NSEvent*)e {
    if (!em34xx::cocoa::modifier_down(e.keyCode, e.modifierFlags)) return;
    const uint32_t hid = em34xx::cocoa::hid_from_keycode(e.keyCode);
    if (hid == 0xE3 || hid == 0xE7) return;   // Command
    [self report:hid flags:e.modifierFlags];
}
@end

@interface EMEntryController : NSObject
@end

@implementation EMEntryController {
  @public
    KeymapEntry _e;
    std::string _originalMods;
    bool _oShift, _oCtrl, _oAlt, _oAny;
    NSPanel* _panel;
    EMKeyCapture* _capture;
    NSComboBox* _host;
    NSButton* _shift;
    NSButton* _ctrl;
    NSButton* _alt;
    NSButton* _any;
    NSComboBox* _to;
    NSPopUpButton* _keyboard;
    NSButton* _tap;
}

- (NSComboBox*)combo:(const std::vector<std::string>&)items {
    NSComboBox* c = [NSComboBox new];
    c.translatesAutoresizingMaskIntoConstraints = NO;
    for (const std::string& s : items) [c addItemWithObjectValue:ns(s)];
    c.numberOfVisibleItems = 16;
    c.completes = YES;
    [c.widthAnchor constraintEqualToConstant:200].active = YES;
    return c;
}

- (void)build {
    _panel = make_panel(@"Key Mapping", NSMakeSize(460, 320));
    _capture = [EMKeyCapture new];
    _capture.translatesAutoresizingMaskIntoConstraints = NO;
    __weak EMEntryController* weak = self;
    _capture.captured = ^(uint32_t hid, bool shift, bool ctrl, bool alt) {
      EMEntryController* c = weak;
      if (!c) return;
      const std::string name = host_key_name(hid);
      c->_host.stringValue = ns(name);
      c->_shift.state = shift ? NSControlStateValueOn : NSControlStateValueOff;
      c->_ctrl.state = ctrl ? NSControlStateValueOn : NSControlStateValueOff;
      c->_alt.state = alt ? NSControlStateValueOn : NSControlStateValueOff;
      c->_capture.text = ns((ctrl ? "Control+" : "") + std::string(alt ? "Option+" : "") +
                            (shift ? "Shift+" : "") + name);
    };
    std::vector<std::string> keys;
    for (uint32_t hid = 0x04; hid <= 0xE7; ++hid) {
        const std::string n = host_key_name(hid);
        if (n.rfind("0x", 0) != 0) keys.push_back(n);
    }
    _host = [self combo:keys];
    _shift = check_box(@"Shift", nil, nil);
    _ctrl = check_box(@"Control", nil, nil);
    _alt = check_box(@"Option (Alt)", nil, nil);
    _any = check_box(@"Any", nil, nil);
    _any.toolTip = @"The mapping applies whatever modifiers are held.";
    std::vector<std::string> to = {"none", "positional"};
    for (const std::string& f : terminal_function_names()) to.push_back(f);
    _to = [self combo:to];
    _keyboard = popup({kKeyboards[0], kKeyboards[1], kKeyboards[2]}, nil, nil);
    _tap = check_box(@"Tap: only when the key is pressed and released alone", nil, nil);
    NSGridView* g = form(@[
        @[ label(@"Capture:"), _capture ],
        @[ label(@"Mac key:"), _host ],
        @[ label(@"Modifiers:"), hstack(@[ _shift, _ctrl, _alt, _any ], 12) ],
        @[ label(@"Terminal key:"), _to ],
        @[ label(@"Keyboard:"), _keyboard ],
        @[ empty(), _tap ],
    ]);
    [g rowAtIndex:1].topPadding = 6;
    [g rowAtIndex:3].topPadding = 6;
    NSButton* cancel = push_button(@"Cancel", self, @selector(cancel:));
    make_cancel(cancel);
    NSButton* ok = push_button(@"OK", self, @selector(ok:));
    make_default(ok);
    NSStackView* content = vstack(@[ g, button_row(@[], @[ cancel, ok ]) ], 16);
    [content.arrangedSubviews.lastObject.widthAnchor constraintEqualToAnchor:content.widthAnchor].active = YES;
    set_content(_panel, content);

    _host.stringValue = ns(_e.host);
    mods_flags(_e.mods, _oShift, _oCtrl, _oAlt, _oAny);
    _originalMods = _e.mods;
    _shift.state = _oShift ? NSControlStateValueOn : NSControlStateValueOff;
    _ctrl.state = _oCtrl ? NSControlStateValueOn : NSControlStateValueOff;
    _alt.state = _oAlt ? NSControlStateValueOn : NSControlStateValueOff;
    _any.state = _oAny ? NSControlStateValueOn : NSControlStateValueOff;
    _to.stringValue = ns(_e.to);
    [_keyboard selectItemAtIndex:0];
    for (NSInteger i = 0; i < 3; ++i)
        if (to_lower(_e.keyboard) == kKeyboardValues[i] || (i == 0 && to_lower(_e.keyboard) == "both"))
            [_keyboard selectItemAtIndex:i];
    _tap.state = _e.tap ? NSControlStateValueOn : NSControlStateValueOff;
    _panel.initialFirstResponder = _capture;
}

- (void)ok:(id)sender {
    (void)sender;
    guarded([&] {
        KeymapEntry e = _e;
        e.host = trim(str(_host.stringValue));
        e.to = trim(str(_to.stringValue));
        const bool shift = _shift.state == NSControlStateValueOn, ctrl = _ctrl.state == NSControlStateValueOn,
                   alt = _alt.state == NSControlStateValueOn, any = _any.state == NSControlStateValueOn;
        if (shift == _oShift && ctrl == _oCtrl && alt == _oAlt && any == _oAny) {
            e.mods = _originalMods;   // keep left/right detail the boxes cannot show
        } else {
            std::string m;
            if (shift) m += "shift ";
            if (ctrl) m += "ctrl ";
            if (alt) m += "alt ";
            if (any) m += "*";
            e.mods = trim(m);
        }
        const NSInteger kb = _keyboard.indexOfSelectedItem;
        e.keyboard = kb > 0 && kb < 3 ? kKeyboardValues[kb] : "";
        e.tap = _tap.state == NSControlStateValueOn;
        std::string error;
        if (!check_keymap_entry(e, error)) {
            error_alert(_panel, "The mapping is not valid.", error);
            return;
        }
        _e = e;
        end_modal(NSModalResponseOK);
    });
}

- (void)cancel:(id)sender {
    (void)sender;
    end_modal(NSModalResponseCancel);
}
@end

namespace {
bool entry_dialog(NSWindow* parent, KeymapEntry& e) {
    EMEntryController* c = [EMEntryController new];
    c->_e = e;
    [c build];
    if (run_sheet(c->_panel, parent) != NSModalResponseOK) return false;
    e = c->_e;
    return true;
}
} // namespace

@interface EMKeymapController : NSObject <NSTableViewDataSource, NSTableViewDelegate>
@end

@implementation EMKeymapController {
  @public
    KeymapFile _file;
    KeymapFile _default;
    NSPanel* _panel;
    NSTableView* _table;
    NSTextField* _base;
    NSSegmentedControl* _addRemove;
    NSButton* _edit;
}

- (void)build {
    _panel = make_panel(@"Keymap", NSMakeSize(640, 440));
    NSTextField* intro = wrapping_label(@"How the Mac keyboard's keys reach the terminal. The keymap applies to "
                                        @"every profile. Command shortcuts always belong to the Mac.",
                                        600);
    _table = [NSTableView new];
    [_table addTableColumn:column(@"host", @"Mac key", 120)];
    [_table addTableColumn:column(@"mods", @"Modifiers", 100)];
    [_table addTableColumn:column(@"to", @"Terminal key", 120)];
    [_table addTableColumn:column(@"keyboard", @"Keyboard", 90)];
    [_table addTableColumn:column(@"options", @"Options", 140)];
    _table.dataSource = self;
    _table.delegate = self;
    _table.target = self;
    _table.doubleAction = @selector(edit:);
    NSScrollView* sv = table_in_scroll(_table, 600, 240);
    _base = wrapping_label(@"", 600);
    _base.textColor = NSColor.secondaryLabelColor;
    _addRemove = [NSSegmentedControl segmentedControlWithImages:@[
        [NSImage imageNamed:NSImageNameAddTemplate], [NSImage imageNamed:NSImageNameRemoveTemplate]
    ]
                                                   trackingMode:NSSegmentSwitchTrackingMomentary
                                                         target:self
                                                         action:@selector(addRemove:)];
    _addRemove.translatesAutoresizingMaskIntoConstraints = NO;
    _edit = push_button(@"Edit…", self, @selector(edit:));
    NSButton* reset = push_button(@"Restore Defaults", self, @selector(reset:));
    NSButton* cancel = push_button(@"Cancel", self, @selector(cancel:));
    make_cancel(cancel);
    NSButton* ok = push_button(@"OK", self, @selector(ok:));
    make_default(ok);
    NSStackView* content =
        vstack(@[ intro, sv, _base, button_row(@[ _addRemove, _edit, reset ], @[ cancel, ok ]) ], 10);
    [content.arrangedSubviews.lastObject.widthAnchor constraintEqualToAnchor:content.widthAnchor].active = YES;
    [sv.widthAnchor constraintEqualToAnchor:content.widthAnchor].active = YES;
    set_content(_panel, content);
    [self fill:0];
}

- (void)fill:(NSInteger)select {
    [_table reloadData];
    select_row(_table, std::min(select, NSInteger(_file.entries.size()) - 1));
    std::string base;
    if (_file.base == "positional")
        base = "Keys not listed press the terminal key at the same position; Shift+F1…F12 enter F13…F24 on the "
               "122-key keyboard.";
    else if (_file.base == "pcomm")
        base = "Keys not listed follow the built-in PCOMM layout (base \"pcomm\").";
    else if (_file.base == "mac")
        base = "Keys not listed follow the built-in Mac layout (base \"mac\").";
    else
        base = "Keys not listed press the terminal key at the same position.";
    _base.stringValue = ns(base);
    [self updateButtons];
}

- (void)updateButtons {
    const bool any = _table.selectedRow >= 0;
    _edit.enabled = any;
    [_addRemove setEnabled:any forSegment:1];
}

- (void)tableViewSelectionDidChange:(NSNotification*)n {
    (void)n;
    [self updateButtons];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView*)t {
    (void)t;
    return NSInteger(_file.entries.size());
}

- (NSView*)tableView:(NSTableView*)t viewForTableColumn:(NSTableColumn*)col row:(NSInteger)row {
    if (row < 0 || size_t(row) >= _file.entries.size()) return nil;
    const KeymapEntry& e = _file.entries[size_t(row)];
    std::string text;
    NSString* id = col.identifier;
    if ([id isEqualToString:@"host"]) text = e.host;
    else if ([id isEqualToString:@"mods"]) text = e.mods.empty() ? std::string("–") : e.mods == "*" ? "any" : e.mods;
    else if ([id isEqualToString:@"to"]) text = e.to;
    else if ([id isEqualToString:@"keyboard"]) text = e.keyboard.empty() ? std::string("both") : e.keyboard;
    else {
        if (e.tap) text += "tap ";
        if (!e.shift.empty()) text += "shift " + e.shift + " ";
        if (!e.alt.empty()) text += "alt " + e.alt;
        text = trim(text);
    }
    return cell_text(t, col, text);
}

- (void)addRemove:(NSSegmentedControl*)sender {
    guarded([&] {
        if (sender.selectedSegment == 0) {
            KeymapEntry e;
            e.host = "F1";
            e.to = "f1";
            if (!entry_dialog(_panel, e)) return;
            _file.entries.push_back(e);
            [self fill:NSInteger(_file.entries.size()) - 1];
        } else {
            const NSInteger sel = _table.selectedRow;
            if (sel < 0 || size_t(sel) >= _file.entries.size()) return;
            _file.entries.erase(_file.entries.begin() + sel);
            [self fill:sel];
        }
    });
}

- (void)edit:(id)sender {
    (void)sender;
    guarded([&] {
        const NSInteger sel = _table.clickedRow >= 0 ? _table.clickedRow : _table.selectedRow;
        if (sel < 0 || size_t(sel) >= _file.entries.size()) return;
        if (entry_dialog(_panel, _file.entries[size_t(sel)])) [self fill:sel];
    });
}

- (void)reset:(id)sender {
    (void)sender;
    guarded([&] {
        if (!ask(_panel, "Restore the default keymap?",
                 "The default is the IBM Personal Communications (PCOMM) layout for Mac keyboards: Return is "
                 "Enter, and the key labelled enter (fn+Return on laptops) is Field Exit.",
                 "Restore Defaults", "Cancel"))
            return;
        _file = _default;
        [self fill:0];
    });
}

- (void)ok:(id)sender {
    (void)sender;
    end_modal(NSModalResponseOK);
}

- (void)cancel:(id)sender {
    (void)sender;
    end_modal(NSModalResponseCancel);
}
@end

bool em34xx::cocoa::keymap_dialog(NSWindow* parent, KeymapFile& file, const KeymapFile& default_file) {
    EMKeymapController* c = [EMKeymapController new];
    c->_file = file;
    c->_default = default_file;
    [c build];
    if (run_sheet(c->_panel, parent) != NSModalResponseOK) return false;
    file = c->_file;
    return true;
}

// ===========================================================================
// About

void em34xx::cocoa::show_about_panel(const UserDirs& dirs) {
    NSMutableAttributedString* credits = [NSMutableAttributedString new];
    NSDictionary* body = @{
        NSFontAttributeName : [NSFont systemFontOfSize:NSFont.smallSystemFontSize],
        NSForegroundColorAttributeName : NSColor.labelColor
    };
    NSMutableParagraphStyle* centred = [NSMutableParagraphStyle new];
    centred.alignment = NSTextAlignmentCenter;
    NSMutableDictionary* head = [body mutableCopy];
    head[NSParagraphStyleAttributeName] = centred;
    const char* tls = tn5250::tls_backend_name();
    const std::string text = "Hardware emulator for IBM 34xx twinax terminals, connected to IBM i over TN5250.\n"
                             "Free software under the GNU GPL, version 3 or later; no warranty.\n\n"
                             "User folder: " + dirs.root() + "\nTLS: " + (tls ? tls : "none") + "\n";
    [credits appendAttributedString:[[NSAttributedString alloc] initWithString:ns(text) attributes:head]];
    const std::string url = EM34XX_HOMEPAGE_URL;
    if (url.empty()) {
        [credits appendAttributedString:[[NSAttributedString alloc] initWithString:@"Project page: to be announced\n"
                                                                        attributes:head]];
    } else {
        NSMutableDictionary* link = [head mutableCopy];
        link[NSLinkAttributeName] = [NSURL URLWithString:ns(url)];
        [credits appendAttributedString:[[NSAttributedString alloc] initWithString:@"Project page: " attributes:head]];
        [credits appendAttributedString:[[NSAttributedString alloc] initWithString:ns(url) attributes:link]];
        [credits appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n" attributes:head]];
    }
    NSString* notices = nil;
    if (NSURL* u = [NSBundle.mainBundle URLForResource:@"notices" withExtension:@"txt"])
        notices = [NSString stringWithContentsOfURL:u encoding:NSUTF8StringEncoding error:nil];
    NSMutableDictionary* mono = [body mutableCopy];
    mono[NSFontAttributeName] = [NSFont monospacedSystemFontOfSize:9 weight:NSFontWeightRegular];
    [credits appendAttributedString:[[NSAttributedString alloc]
                                        initWithString:[@"\n" stringByAppendingString:notices ? notices : @"(notices not found)"]
                                            attributes:mono]];
    [NSApp orderFrontStandardAboutPanelWithOptions:@{
        NSAboutPanelOptionApplicationName : @"em34xx",
        NSAboutPanelOptionApplicationVersion : @EM34XX_VERSION_STRING,
        NSAboutPanelOptionVersion : @"",
        NSAboutPanelOptionCredits : credits,
    }];
}
