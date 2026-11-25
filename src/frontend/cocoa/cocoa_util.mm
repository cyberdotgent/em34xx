// SPDX-License-Identifier: GPL-3.0-or-later
#import "frontend/cocoa/cocoa_util.h"

#import <Carbon/Carbon.h>   // kVK_* key codes, KBGetLayoutType

#include "util/log.hpp"

#include <cxxabi.h>
#include <exception>
#include <filesystem>
#include <system_error>
#include <typeinfo>

namespace em34xx {
namespace cocoa {

NSString* ns(const std::string& utf8) {
    NSString* s = [[NSString alloc] initWithBytes:utf8.data() length:utf8.size() encoding:NSUTF8StringEncoding];
    return s ? s : @"";
}

std::string str(NSString* s) {
    if (!s) return std::string();
    const char* c = s.UTF8String;
    return c ? std::string(c) : std::string();
}

// --- modal sheets ----------------------------------------------------------------

NSWindow* sheet_host(NSWindow* w) {
    while (w && w.attachedSheet) w = w.attachedSheet;
    return w;
}

NSModalResponse run_sheet(NSWindow* sheet, NSWindow* parent) {
    NSWindow* host = sheet_host(parent);
    NSModalResponse r;
    if (host && host.visible && !host.miniaturized) {
        [host beginSheet:sheet completionHandler:nil];
        r = [NSApp runModalForWindow:sheet];
        [host endSheet:sheet returnCode:r];
    } else {
        [sheet center];
        r = [NSApp runModalForWindow:sheet];
    }
    [sheet orderOut:nil];
    return r;
}

void end_modal(NSModalResponse code) { [NSApp stopModalWithCode:code]; }

bool modal_ui_active(NSWindow* main) { return NSApp.modalWindow != nil || (main && main.attachedSheet != nil); }

// --- alerts -----------------------------------------------------------------------

NSModalResponse run_alert(NSAlert* alert, NSWindow* parent) {
    NSWindow* host = sheet_host(parent);
    if (host && host.visible && !host.miniaturized) {
        __block NSModalResponse result = NSAlertFirstButtonReturn;
        [alert beginSheetModalForWindow:host
                      completionHandler:^(NSModalResponse r) {
                        result = r;
                        [NSApp stopModalWithCode:r];
                      }];
        [NSApp runModalForWindow:alert.window];
        return result;
    }
    return [alert runModal];
}

namespace {
NSAlert* make_alert(NSAlertStyle style, const std::string& message, const std::string& detail) {
    NSAlert* a = [NSAlert new];
    a.alertStyle = style;
    a.messageText = ns(message);
    a.informativeText = ns(detail);
    return a;
}
} // namespace

void error_alert(NSWindow* parent, const std::string& message, const std::string& detail) {
    NSAlert* a = make_alert(NSAlertStyleCritical, message, detail);
    [a addButtonWithTitle:@"OK"];
    run_alert(a, parent);
}

void info_alert(NSWindow* parent, const std::string& message, const std::string& detail) {
    NSAlert* a = make_alert(NSAlertStyleInformational, message, detail);
    [a addButtonWithTitle:@"OK"];
    run_alert(a, parent);
}

bool ask(NSWindow* parent, const std::string& message, const std::string& detail, const std::string& yes,
         const std::string& no, NSAlertStyle style, bool no_is_default) {
    NSAlert* a = make_alert(style, message, detail);
    if (no_is_default) {
        [a addButtonWithTitle:ns(no)];
        NSButton* y = [a addButtonWithTitle:ns(yes)];
        y.keyEquivalent = @"";
        return run_alert(a, parent) == NSAlertSecondButtonReturn;
    }
    [a addButtonWithTitle:ns(yes)];
    NSButton* n = [a addButtonWithTitle:ns(no)];
    n.keyEquivalent = @"\x1b";
    return run_alert(a, parent) == NSAlertFirstButtonReturn;
}

// --- building windows ------------------------------------------------------------

NSPanel* make_panel(NSString* title, NSSize content) {
    NSPanel* p = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, content.width, content.height)
                                            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                              backing:NSBackingStoreBuffered
                                                defer:YES];
    p.title = title;
    p.releasedWhenClosed = NO;
    p.hidesOnDeactivate = NO;
    return p;
}

NSTextField* label(NSString* text) {
    NSTextField* t = [NSTextField labelWithString:text];
    t.translatesAutoresizingMaskIntoConstraints = NO;
    return t;
}

NSTextField* small_label(NSString* text) {
    NSTextField* t = label(text);
    t.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    t.textColor = NSColor.secondaryLabelColor;
    return t;
}

NSTextField* wrapping_label(NSString* text, CGFloat width) {
    NSTextField* t = [NSTextField wrappingLabelWithString:text];
    t.translatesAutoresizingMaskIntoConstraints = NO;
    t.preferredMaxLayoutWidth = width;
    [t.widthAnchor constraintEqualToConstant:width].active = YES;
    t.selectable = YES;
    return t;
}

NSTextField* text_field(NSString* text, CGFloat width) {
    NSTextField* t = [NSTextField textFieldWithString:text ? text : @""];
    t.translatesAutoresizingMaskIntoConstraints = NO;
    if (width > 0) [t.widthAnchor constraintEqualToConstant:width].active = YES;
    return t;
}

NSButton* push_button(NSString* title, id target, SEL action) {
    NSButton* b = [NSButton buttonWithTitle:title target:target action:action];
    b.translatesAutoresizingMaskIntoConstraints = NO;
    return b;
}

NSButton* check_box(NSString* title, id target, SEL action) {
    NSButton* b = [NSButton checkboxWithTitle:title target:target action:action];
    b.translatesAutoresizingMaskIntoConstraints = NO;
    return b;
}

NSButton* radio(NSString* title, id target, SEL action) {
    NSButton* b = [NSButton radioButtonWithTitle:title target:target action:action];
    b.translatesAutoresizingMaskIntoConstraints = NO;
    return b;
}

NSPopUpButton* popup(const std::vector<std::string>& items, id target, SEL action) {
    NSPopUpButton* p = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    p.translatesAutoresizingMaskIntoConstraints = NO;
    for (const std::string& i : items) {
        // addItemWithTitle: drops duplicates; menu items keep them all.
        [p.menu addItem:[[NSMenuItem alloc] initWithTitle:ns(i) action:nil keyEquivalent:@""]];
    }
    p.target = target;
    p.action = action;
    return p;
}

NSScrollView* text_view(CGFloat width, CGFloat height, NSTextView** out_view) {
    NSScrollView* sv = [NSTextView scrollableTextView];
    sv.translatesAutoresizingMaskIntoConstraints = NO;
    sv.borderType = NSBezelBorder;
    sv.hasVerticalScroller = YES;
    [sv.widthAnchor constraintEqualToConstant:width].active = YES;
    [sv.heightAnchor constraintEqualToConstant:height].active = YES;
    NSTextView* tv = sv.documentView;
    tv.editable = NO;
    tv.selectable = YES;
    tv.richText = NO;
    tv.font = [NSFont monospacedSystemFontOfSize:NSFont.smallSystemFontSize weight:NSFontWeightRegular];
    tv.textContainerInset = NSMakeSize(4, 4);
    if (out_view) *out_view = tv;
    return sv;
}

void make_default(NSButton* b) { b.keyEquivalent = @"\r"; }
void make_cancel(NSButton* b) { b.keyEquivalent = @"\x1b"; }

NSStackView* vstack(NSArray<NSView*>* views, CGFloat spacing) {
    NSStackView* s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationVertical;
    s.alignment = NSLayoutAttributeLeading;
    s.spacing = spacing;
    s.translatesAutoresizingMaskIntoConstraints = NO;
    return s;
}

NSStackView* hstack(NSArray<NSView*>* views, CGFloat spacing) {
    NSStackView* s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    s.alignment = NSLayoutAttributeFirstBaseline;
    s.spacing = spacing;
    s.translatesAutoresizingMaskIntoConstraints = NO;
    return s;
}

NSStackView* button_row(NSArray<NSView*>* left, NSArray<NSView*>* right) {
    NSStackView* row = [NSStackView new];
    row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row.spacing = 12;
    row.translatesAutoresizingMaskIntoConstraints = NO;
    for (NSView* v in left) [row addView:v inGravity:NSStackViewGravityLeading];
    for (NSView* v in right) [row addView:v inGravity:NSStackViewGravityTrailing];
    return row;
}

void set_content(NSWindow* window, NSView* content) {
    NSView* root = [NSView new];
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [root addSubview:content];
    [NSLayoutConstraint activateConstraints:@[
        [content.topAnchor constraintEqualToAnchor:root.topAnchor constant:20],
        [content.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:20],
        [content.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-20],
        [content.bottomAnchor constraintEqualToAnchor:root.bottomAnchor constant:-20],
    ]];
    window.contentView = root;
    [root layoutSubtreeIfNeeded];
    [window setContentSize:root.fittingSize];
}

// --- keyboard ----------------------------------------------------------------------

namespace {
struct KeyHid {
    unsigned short key;
    uint8_t hid;
};
// macOS virtual key codes (HIToolbox Events.h) -> USB HID usages (HID Usage
// Tables, Keyboard/Keypad page 0x07), by position on the keyboard.
const KeyHid kKeyTable[] = {
    {kVK_ANSI_A, 0x04}, {kVK_ANSI_B, 0x05}, {kVK_ANSI_C, 0x06}, {kVK_ANSI_D, 0x07}, {kVK_ANSI_E, 0x08},
    {kVK_ANSI_F, 0x09}, {kVK_ANSI_G, 0x0A}, {kVK_ANSI_H, 0x0B}, {kVK_ANSI_I, 0x0C}, {kVK_ANSI_J, 0x0D},
    {kVK_ANSI_K, 0x0E}, {kVK_ANSI_L, 0x0F}, {kVK_ANSI_M, 0x10}, {kVK_ANSI_N, 0x11}, {kVK_ANSI_O, 0x12},
    {kVK_ANSI_P, 0x13}, {kVK_ANSI_Q, 0x14}, {kVK_ANSI_R, 0x15}, {kVK_ANSI_S, 0x16}, {kVK_ANSI_T, 0x17},
    {kVK_ANSI_U, 0x18}, {kVK_ANSI_V, 0x19}, {kVK_ANSI_W, 0x1A}, {kVK_ANSI_X, 0x1B}, {kVK_ANSI_Y, 0x1C},
    {kVK_ANSI_Z, 0x1D}, {kVK_ANSI_1, 0x1E}, {kVK_ANSI_2, 0x1F}, {kVK_ANSI_3, 0x20}, {kVK_ANSI_4, 0x21},
    {kVK_ANSI_5, 0x22}, {kVK_ANSI_6, 0x23}, {kVK_ANSI_7, 0x24}, {kVK_ANSI_8, 0x25}, {kVK_ANSI_9, 0x26},
    {kVK_ANSI_0, 0x27}, {kVK_Return, 0x28}, {kVK_Escape, 0x29}, {kVK_Delete, 0x2A}, {kVK_Tab, 0x2B},
    {kVK_Space, 0x2C}, {kVK_ANSI_Minus, 0x2D}, {kVK_ANSI_Equal, 0x2E}, {kVK_ANSI_LeftBracket, 0x2F},
    {kVK_ANSI_RightBracket, 0x30}, {kVK_ANSI_Backslash, 0x31}, {kVK_ANSI_Semicolon, 0x33},
    {kVK_ANSI_Quote, 0x34}, {kVK_ANSI_Grave, 0x35}, {kVK_ANSI_Comma, 0x36}, {kVK_ANSI_Period, 0x37},
    {kVK_ANSI_Slash, 0x38}, {kVK_CapsLock, 0x39},
    {kVK_F1, 0x3A}, {kVK_F2, 0x3B}, {kVK_F3, 0x3C}, {kVK_F4, 0x3D}, {kVK_F5, 0x3E}, {kVK_F6, 0x3F},
    {kVK_F7, 0x40}, {kVK_F8, 0x41}, {kVK_F9, 0x42}, {kVK_F10, 0x43}, {kVK_F11, 0x44}, {kVK_F12, 0x45},
    // The Help key of Apple's extended keyboards sits where Insert is.
    {kVK_Help, 0x49}, {kVK_Home, 0x4A}, {kVK_PageUp, 0x4B}, {kVK_ForwardDelete, 0x4C}, {kVK_End, 0x4D},
    {kVK_PageDown, 0x4E}, {kVK_RightArrow, 0x4F}, {kVK_LeftArrow, 0x50}, {kVK_DownArrow, 0x51},
    {kVK_UpArrow, 0x52},
    // Keypad Clear is where Num Lock is.
    {kVK_ANSI_KeypadClear, 0x53}, {kVK_ANSI_KeypadDivide, 0x54}, {kVK_ANSI_KeypadMultiply, 0x55},
    {kVK_ANSI_KeypadMinus, 0x56}, {kVK_ANSI_KeypadPlus, 0x57}, {kVK_ANSI_KeypadEnter, 0x58},
    {kVK_ANSI_Keypad1, 0x59}, {kVK_ANSI_Keypad2, 0x5A}, {kVK_ANSI_Keypad3, 0x5B}, {kVK_ANSI_Keypad4, 0x5C},
    {kVK_ANSI_Keypad5, 0x5D}, {kVK_ANSI_Keypad6, 0x5E}, {kVK_ANSI_Keypad7, 0x5F}, {kVK_ANSI_Keypad8, 0x60},
    {kVK_ANSI_Keypad9, 0x61}, {kVK_ANSI_Keypad0, 0x62}, {kVK_ANSI_KeypadDecimal, 0x63},
    {kVK_ANSI_KeypadEquals, 0x67},
    {kVK_F13, 0x68}, {kVK_F14, 0x69}, {kVK_F15, 0x6A}, {kVK_F16, 0x6B}, {kVK_F17, 0x6C}, {kVK_F18, 0x6D},
    {kVK_F19, 0x6E}, {kVK_F20, 0x6F},
    {0x6E, 0x65},   // the contextual-menu (Application) key of PC keyboards
    {kVK_JIS_KeypadComma, 0x85}, {kVK_JIS_Underscore, 0x87}, {kVK_JIS_Yen, 0x89}, {kVK_JIS_Kana, 0x90},
    {kVK_JIS_Eisu, 0x91},
    {kVK_Control, 0xE0}, {kVK_Shift, 0xE1}, {kVK_Option, 0xE2}, {kVK_Command, 0xE3},
    {kVK_RightControl, 0xE4}, {kVK_RightShift, 0xE5}, {kVK_RightOption, 0xE6}, {kVK_RightCommand, 0xE7},
};

bool iso_keyboard() { return KBGetLayoutType(LMGetKbdType()) == kKeyboardISO; }
} // namespace

uint32_t hid_from_keycode(unsigned short key_code) {
    // ISO keyboards: the key left of 1 reports kVK_ISO_Section and the key
    // left of Z reports kVK_ANSI_Grave (macOS swaps them); by position they
    // are Grave (0x35) and Non-US Backslash (0x64).
    if (key_code == kVK_ISO_Section) return iso_keyboard() ? 0x35 : 0x64;
    if (key_code == kVK_ANSI_Grave && iso_keyboard()) return 0x64;
    for (const KeyHid& k : kKeyTable)
        if (k.key == key_code) return k.hid;
    return 0;
}

bool modifier_down(unsigned short key_code, NSEventModifierFlags flags) {
    // Device-dependent bits (IOKit NX_DEVICE*KEYMASK) tell left from right;
    // a keyboard that does not report them gets the generic flag.
    struct Mod {
        unsigned short key;
        NSEventModifierFlags device, generic;
    };
    static const Mod kMods[] = {
        {kVK_Control, 0x0001, NSEventModifierFlagControl},  {kVK_RightControl, 0x2000, NSEventModifierFlagControl},
        {kVK_Shift, 0x0002, NSEventModifierFlagShift},      {kVK_RightShift, 0x0004, NSEventModifierFlagShift},
        {kVK_Command, 0x0008, NSEventModifierFlagCommand},  {kVK_RightCommand, 0x0010, NSEventModifierFlagCommand},
        {kVK_Option, 0x0020, NSEventModifierFlagOption},    {kVK_RightOption, 0x0040, NSEventModifierFlagOption},
    };
    const NSEventModifierFlags all_device = 0x0001 | 0x2000 | 0x0002 | 0x0004 | 0x0008 | 0x0010 | 0x0020 | 0x0040;
    for (const Mod& m : kMods) {
        if (m.key != key_code) continue;
        if (flags & all_device) return (flags & m.device) != 0;
        return (flags & m.generic) != 0;
    }
    return false;
}

// --- files --------------------------------------------------------------------------

namespace {
std::string library_folder(NSSearchPathDirectory which) {
    NSURL* url = [NSFileManager.defaultManager URLForDirectory:which
                                                      inDomain:NSUserDomainMask
                                             appropriateForURL:nil
                                                        create:YES
                                                         error:nil];
    return url ? str(url.path) : std::string();
}
} // namespace

std::string application_support_folder() { return library_folder(NSApplicationSupportDirectory); }

std::string logs_folder() {
    const std::string lib = library_folder(NSLibraryDirectory);
    return lib.empty() ? lib : lib + "/Logs";
}

bool file_in_folder(const std::string& file, const std::string& folder) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::absolute(fs::u8path(file), ec).parent_path();
    if (ec) return false;
    const bool same = fs::equivalent(dir, fs::u8path(folder), ec);
    return !ec && same;
}

void reveal_in_finder(const std::string& path) {
    [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[ [NSURL fileURLWithPath:ns(path)] ]];
}

// --- exceptions -------------------------------------------------------------------

void report_exception(const char* what) noexcept {
    static bool showing = false;
    if (showing) return;
    showing = true;
    @try {
        NSAlert* a = [NSAlert new];
        a.alertStyle = NSAlertStyleCritical;
        a.messageText = @"em34xx ran into an unexpected error.";
        a.informativeText = [NSString
            stringWithFormat:@"%@\n\nThe operation was abandoned; em34xx keeps running.",
                             what && *what && [NSString stringWithUTF8String:what]
                                 ? [NSString stringWithUTF8String:what]
                                 : @"(no description)"];
        [a addButtonWithTitle:@"OK"];
        [a runModal];
    } @catch (NSException*) {
    }
    showing = false;
}

namespace {
// A C++ type name as written in the source, or the mangled one.
std::string type_name(const std::type_info* t) {
    if (!t) return "unknown type";
    int status = 0;
    char* d = abi::__cxa_demangle(t->name(), nullptr, nullptr, &status);
    std::string out = status == 0 && d ? d : t->name();
    std::free(d);
    return out;
}
} // namespace

void report_current_exception() noexcept {
    std::string what;
    try {
        try {
            throw;
        } catch (NSException* e) {
            what = "Objective-C exception " + str(e.name) + ": " + str(e.reason);
            log_write("cocoa", LogLevel::Error, "%s", what.c_str());
            for (NSString* line in e.callStackSymbols) log_write("cocoa", LogLevel::Error, "  %s", str(line).c_str());
        } catch (const std::exception& e) {
            what = e.what() && *e.what() ? std::string(e.what()) : "(no description)";
            what += " [" + type_name(abi::__cxa_current_exception_type()) + "]";
            log_write("cocoa", LogLevel::Error, "unexpected exception: %s", what.c_str());
        } catch (...) {
            what = "Exception of type " + type_name(abi::__cxa_current_exception_type());
            log_write("cocoa", LogLevel::Error, "unexpected exception: %s", what.c_str());
        }
    } catch (...) {
        // Out of memory while describing it: report it bare.
        report_exception(nullptr);
        return;
    }
    report_exception(what.c_str());
}

} // namespace cocoa
} // namespace em34xx
