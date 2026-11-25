// SPDX-License-Identifier: GPL-3.0-or-later
// Small AppKit helpers for em34xx.app: UTF-8 <-> NSString, modal sheets run
// to completion (the dialogs are written as functions that return the
// user's answer, like their Win32 counterparts), alerts, Auto Layout form
// building, the macOS virtual key code -> USB HID usage table, and the
// exception guard for AppKit callbacks. Objective-C++ (ARC).
#pragma once

#import <Cocoa/Cocoa.h>

#include <cstdint>
#include <exception>
#include <string>
#include <vector>

namespace em34xx {
namespace cocoa {

NSString* ns(const std::string& utf8);
std::string str(NSString* s);

// --- modal sheets ------------------------------------------------------------
// Attach `sheet` to `parent` (stacked on the sheet `parent` already shows, if
// any) and run it modally until a button calls end_modal(). Without a
// visible parent the window runs app-modal, centred on the screen. The
// sheet is ordered out before this returns. Display updates keep running
// (the main queue and common-mode timers are serviced in the modal loop).
NSModalResponse run_sheet(NSWindow* sheet, NSWindow* parent);
// From a button in a window shown by run_sheet().
void end_modal(NSModalResponse code);
// The window a new sheet over `w` would attach to (the deepest sheet).
NSWindow* sheet_host(NSWindow* w);
// Whether a sheet, modal window or alert is up (the main window then stays
// as it is: the TLS trust prompt waits until it is gone).
bool modal_ui_active(NSWindow* main);

// --- alerts -----------------------------------------------------------------
// An alert as a sheet on `parent` (or app-modal without one); returns
// NSAlertFirstButtonReturn + index of the button pressed.
NSModalResponse run_alert(NSAlert* alert, NSWindow* parent);
void error_alert(NSWindow* parent, const std::string& message, const std::string& detail = std::string());
void info_alert(NSWindow* parent, const std::string& message, const std::string& detail = std::string());
// A question with two buttons; true when `yes` was pressed. `yes` is the
// default (Return) and `no` answers Escape, unless `no_is_default`: then
// `no` is the default, the safe answer for a risky question.
bool ask(NSWindow* parent, const std::string& message, const std::string& detail, const std::string& yes,
         const std::string& no, NSAlertStyle style = NSAlertStyleInformational, bool no_is_default = false);

// --- building windows in code (Auto Layout) --------------------------------
NSPanel* make_panel(NSString* title, NSSize content);
NSTextField* label(NSString* text);
NSTextField* small_label(NSString* text);
NSTextField* wrapping_label(NSString* text, CGFloat width);
NSTextField* text_field(NSString* text, CGFloat width);
NSButton* push_button(NSString* title, id target, SEL action);
NSButton* check_box(NSString* title, id target, SEL action);
NSButton* radio(NSString* title, id target, SEL action);
NSPopUpButton* popup(const std::vector<std::string>& items, id target, SEL action);
// A read-only, selectable, monospaced text area in a scroll view.
NSScrollView* text_view(CGFloat width, CGFloat height, NSTextView** out_view);
// The Return / Escape key equivalents of the default and cancel buttons.
void make_default(NSButton* b);
void make_cancel(NSButton* b);
// A right-aligned row of buttons (cancel first, default last, as AppKit
// lays them out), with optional buttons on the left.
NSStackView* button_row(NSArray<NSView*>* left, NSArray<NSView*>* right);
// Put `content` in `window` with standard margins (20 pt) and let the
// window size itself to it.
void set_content(NSWindow* window, NSView* content);
NSStackView* vstack(NSArray<NSView*>* views, CGFloat spacing = 8);
NSStackView* hstack(NSArray<NSView*>* views, CGFloat spacing = 8);

// --- keyboard -----------------------------------------------------------------
// USB HID usage (page 7) of a macOS virtual key code (NSEvent.keyCode,
// kVK_*), by position; ISO keyboards swap the codes of the two keys left of
// 1 and left of Z, which this undoes. 0 for keys without one (Fn, Command
// is reported but not sent, volume keys).
uint32_t hid_from_keycode(unsigned short key_code);
// For flagsChanged: whether the modifier key `key_code` is now down, from
// the event's device-dependent flags. Caps Lock is a toggle (see caller).
bool modifier_down(unsigned short key_code, NSEventModifierFlags flags);

// --- files ------------------------------------------------------------------
// The user's Application Support folder (UTF-8), empty when unavailable.
std::string application_support_folder();
// ~/Library/Logs (UTF-8), empty when unavailable.
std::string logs_folder();
// Whether `file` is directly in `folder`: the file's directory and `folder`
// are the same directory on disk, whatever the spelling (symlinks, case,
// relative paths). False when either cannot be resolved.
bool file_in_folder(const std::string& file, const std::string& folder);
void reveal_in_finder(const std::string& path);

// --- exceptions ---------------------------------------------------------------
// No C++ exception may unwind through AppKit frames: actions, timers and
// dispatched blocks run their body through guarded(), which reports the
// exception in an alert (one at a time) and returns `fallback`. catch (...)
// also catches Objective-C exceptions (NSException): the report names
// whichever it was and writes it, with an NSException's backtrace, to the
// log.
void report_exception(const char* what) noexcept;
// Call from a catch block only: reports the exception being handled.
void report_current_exception() noexcept;
template <class R, class F>
R guarded(R fallback, F&& body) noexcept {
    try {
        return body();
    } catch (...) {
        report_current_exception();
    }
    return fallback;
}
template <class F>
void guarded(F&& body) noexcept {
    try {
        body();
    } catch (...) {
        report_current_exception();
    }
}

} // namespace cocoa
} // namespace em34xx
