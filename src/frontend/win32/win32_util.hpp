// SPDX-License-Identifier: GPL-3.0-or-later
// Small Win32 helpers for tn34xxw: UTF-8 <-> UTF-16, DPI (newer APIs loaded
// at run time so the program still starts on Windows 7), the keyboard scan
// code -> USB HID usage table, message boxes, file dialogs.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace tn34xx {
namespace win32 {

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& wide);

// The path as the C runtime's narrow functions (fopen, ifstream) can open
// it: unchanged when ASCII, else its 8.3 short form when there is one, else
// converted to the ANSI code page. The emulator core opens files by narrow
// name; this keeps non-ASCII user folders working where Windows allows.
std::string runtime_path(const std::string& utf8);

// --- DPI ------------------------------------------------------------------
// GetDpiForWindow (Windows 10 1607+), else the system DPI.
UINT window_dpi(HWND hwnd);
UINT system_dpi();
inline int scale_px(int px, UINT dpi) { return MulDiv(px, int(dpi), 96); }
// AdjustWindowRectExForDpi when available.
void adjust_window_rect(RECT& r, DWORD style, BOOL menu, DWORD ex_style, UINT dpi);
// Menu / caption font for a DPI (SystemParametersInfoForDpi when available).
HFONT create_ui_font(UINT dpi);

// --- keyboard ---------------------------------------------------------------
// USB HID usage (page 7) of a WM_KEYDOWN/UP: scan code in lParam bits 16-23,
// extended flag bit 24, VK for the ambiguous keys (Pause / Num Lock).
// 0 when unknown.
uint32_t hid_from_key_message(WPARAM vk, LPARAM lparam);

// --- UI -----------------------------------------------------------------------
std::wstring load_string(UINT id);
// A string-table entry with its "%s" replaced by `arg`, as UTF-8.
std::string string_with(UINT id, const std::string& arg);
int message_box(HWND owner, const std::string& text, UINT flags);
void error_box(HWND owner, const std::string& text);
// Common open-file dialog with multiple selection; UTF-8 paths.
std::vector<std::string> open_files_dialog(HWND owner, const std::wstring& title, const std::wstring& filter_pipes);
// Text of a control as UTF-8 / set it.
std::string get_text(HWND dlg, int id);
void set_text(HWND dlg, int id, const std::string& text);
// SHGetFolderPathW(CSIDL_APPDATA) as UTF-8 (Windows 7: no Known Folders needed).
std::string appdata_folder();
std::string module_path();

} // namespace win32
} // namespace tn34xx
