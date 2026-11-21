#include "frontend/win32/win32_util.hpp"

#include <commdlg.h>
#include <shlobj.h>

#include <array>

namespace tn34xx {
namespace win32 {

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

namespace {
bool is_ascii(const std::string& s) {
    for (unsigned char c : s)
        if (c >= 0x80) return false;
    return true;
}
std::string to_acp(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 1 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}
} // namespace

std::string runtime_path(const std::string& utf8) {
    if (is_ascii(utf8)) return utf8;
    const std::wstring w = widen(utf8);
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetShortPathNameW(w.c_str(), buf, DWORD(sizeof buf / sizeof buf[0]));
    if (n > 0 && n < sizeof buf / sizeof buf[0]) {
        const std::string s = narrow(buf);
        if (is_ascii(s)) return s;
    }
    // A file that does not exist yet: shorten its folder.
    const size_t slash = w.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        n = GetShortPathNameW(w.substr(0, slash).c_str(), buf, DWORD(sizeof buf / sizeof buf[0]));
        if (n > 0 && n < sizeof buf / sizeof buf[0]) {
            const std::wstring tail = w.substr(slash);
            const std::string s = narrow(std::wstring(buf) + tail);
            if (is_ascii(s)) return s;
            return to_acp(std::wstring(buf) + tail);
        }
    }
    return to_acp(w);
}

// --- DPI ------------------------------------------------------------------------

namespace {
template <class F>
F user32_proc(const char* name) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    return user32 ? reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(user32, name))) : nullptr;
}
} // namespace

UINT system_dpi() {
    HDC dc = GetDC(nullptr);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return dpi > 0 ? UINT(dpi) : 96u;
}

UINT window_dpi(HWND hwnd) {
    using Fn = UINT(WINAPI*)(HWND);
    static const Fn fn = user32_proc<Fn>("GetDpiForWindow");
    if (fn && hwnd) {
        const UINT d = fn(hwnd);
        if (d) return d;
    }
    return system_dpi();
}

void adjust_window_rect(RECT& r, DWORD style, BOOL menu, DWORD ex_style, UINT dpi) {
    using Fn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static const Fn fn = user32_proc<Fn>("AdjustWindowRectExForDpi");
    if (fn && fn(&r, style, menu, ex_style, dpi)) return;
    AdjustWindowRectEx(&r, style, menu, ex_style);
}

HFONT create_ui_font(UINT dpi) {
    NONCLIENTMETRICSW ncm;
    ZeroMemory(&ncm, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    using Fn = BOOL(WINAPI*)(UINT, UINT, PVOID, UINT, UINT);
    static const Fn fn = user32_proc<Fn>("SystemParametersInfoForDpi");
    if (fn && fn(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi)) return CreateFontIndirectW(&ncm.lfMessageFont);
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0)) return nullptr;
    // Scale the system-DPI font to this DPI.
    ncm.lfMessageFont.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, int(dpi), int(system_dpi()));
    return CreateFontIndirectW(&ncm.lfMessageFont);
}

// --- keyboard ---------------------------------------------------------------------

namespace {

// Scan-code set 1 make codes (as Windows reports them) -> USB HID usages,
// for keys without and with the E0 (extended) prefix.
struct ScanTables {
    std::array<uint8_t, 128> plain{};
    std::array<uint8_t, 128> ext{};
    ScanTables() {
        static const uint8_t kPlain[][2] = {
            {0x01, 0x29}, {0x02, 0x1E}, {0x03, 0x1F}, {0x04, 0x20}, {0x05, 0x21}, {0x06, 0x22}, {0x07, 0x23},
            {0x08, 0x24}, {0x09, 0x25}, {0x0A, 0x26}, {0x0B, 0x27}, {0x0C, 0x2D}, {0x0D, 0x2E}, {0x0E, 0x2A},
            {0x0F, 0x2B}, {0x10, 0x14}, {0x11, 0x1A}, {0x12, 0x08}, {0x13, 0x15}, {0x14, 0x17}, {0x15, 0x1C},
            {0x16, 0x18}, {0x17, 0x0C}, {0x18, 0x12}, {0x19, 0x13}, {0x1A, 0x2F}, {0x1B, 0x30}, {0x1C, 0x28},
            {0x1D, 0xE0}, {0x1E, 0x04}, {0x1F, 0x16}, {0x20, 0x07}, {0x21, 0x09}, {0x22, 0x0A}, {0x23, 0x0B},
            {0x24, 0x0D}, {0x25, 0x0E}, {0x26, 0x0F}, {0x27, 0x33}, {0x28, 0x34}, {0x29, 0x35}, {0x2A, 0xE1},
            {0x2B, 0x31}, {0x2C, 0x1D}, {0x2D, 0x1B}, {0x2E, 0x06}, {0x2F, 0x19}, {0x30, 0x05}, {0x31, 0x11},
            {0x32, 0x10}, {0x33, 0x36}, {0x34, 0x37}, {0x35, 0x38}, {0x36, 0xE5}, {0x37, 0x55}, {0x38, 0xE2},
            {0x39, 0x2C}, {0x3A, 0x39}, {0x3B, 0x3A}, {0x3C, 0x3B}, {0x3D, 0x3C}, {0x3E, 0x3D}, {0x3F, 0x3E},
            {0x40, 0x3F}, {0x41, 0x40}, {0x42, 0x41}, {0x43, 0x42}, {0x44, 0x43}, {0x45, 0x48}, {0x46, 0x47},
            {0x47, 0x5F}, {0x48, 0x60}, {0x49, 0x61}, {0x4A, 0x56}, {0x4B, 0x5C}, {0x4C, 0x5D}, {0x4D, 0x5E},
            {0x4E, 0x57}, {0x4F, 0x59}, {0x50, 0x5A}, {0x51, 0x5B}, {0x52, 0x62}, {0x53, 0x63}, {0x54, 0x46},
            {0x56, 0x64}, {0x57, 0x44}, {0x58, 0x45}, {0x59, 0x67}, {0x64, 0x68}, {0x65, 0x69}, {0x66, 0x6A},
            {0x67, 0x6B}, {0x68, 0x6C}, {0x69, 0x6D}, {0x6A, 0x6E}, {0x6B, 0x6F}, {0x6C, 0x70}, {0x6D, 0x71},
            {0x6E, 0x72}, {0x70, 0x88}, {0x73, 0x87}, {0x76, 0x73}, {0x79, 0x8A}, {0x7B, 0x8B}, {0x7D, 0x89},
            {0x7E, 0x85},
        };
        static const uint8_t kExt[][2] = {
            {0x1C, 0x58}, {0x1D, 0xE4}, {0x35, 0x54}, {0x37, 0x46}, {0x38, 0xE6}, {0x45, 0x53}, {0x46, 0x48},
            {0x47, 0x4A}, {0x48, 0x52}, {0x49, 0x4B}, {0x4B, 0x50}, {0x4D, 0x4F}, {0x4F, 0x4D}, {0x50, 0x51},
            {0x51, 0x4E}, {0x52, 0x49}, {0x53, 0x4C}, {0x5B, 0xE3}, {0x5C, 0xE7}, {0x5D, 0x65},
        };
        for (const auto& e : kPlain) plain[e[0]] = e[1];
        for (const auto& e : kExt) ext[e[0]] = e[1];
    }
};

} // namespace

uint32_t hid_from_key_message(WPARAM vk, LPARAM lparam) {
    static const ScanTables tables;
    // Pause and Num Lock share scan code 45 (Pause arrives as E1 1D 45);
    // Windows tells them apart by the virtual key.
    if (vk == VK_PAUSE) return 0x48;
    if (vk == VK_NUMLOCK) return 0x53;
    UINT scan = UINT((lparam >> 16) & 0xFF);
    const bool extended = (lparam >> 24) & 1;
    if (scan == 0) scan = MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_VSC);   // synthesised keys
    if (scan >= 128) return 0;
    return extended ? tables.ext[scan] : tables.plain[scan];
}

// --- UI -------------------------------------------------------------------------

std::wstring load_string(UINT id) {
    const wchar_t* p = nullptr;
    const int n = LoadStringW(GetModuleHandleW(nullptr), id, reinterpret_cast<LPWSTR>(&p), 0);
    return n > 0 && p ? std::wstring(p, size_t(n)) : std::wstring();
}

std::string string_with(UINT id, const std::string& arg) {
    std::string s = narrow(load_string(id));
    const size_t at = s.find("%s");
    if (at != std::string::npos) s.replace(at, 2, arg);
    return s;
}

int message_box(HWND owner, const std::string& text, UINT flags) {
    return MessageBoxW(owner, widen(text).c_str(), load_string(4000 /* IDS_APP_TITLE */).c_str(), flags);
}

void error_box(HWND owner, const std::string& text) { message_box(owner, text, MB_OK | MB_ICONERROR); }

std::vector<std::string> open_files_dialog(HWND owner, const std::wstring& title, const std::wstring& filter_pipes) {
    std::wstring filter = filter_pipes;
    for (wchar_t& c : filter)
        if (c == L'|') c = L'\0';
    filter.push_back(L'\0');
    std::vector<wchar_t> buf(32768, L'\0');
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = DWORD(buf.size());
    ofn.lpstrTitle = title.c_str();
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY |
                OFN_NOCHANGEDIR;
    std::vector<std::string> out;
    if (!GetOpenFileNameW(&ofn)) return out;
    // One file: the full path. Several: the folder, then the names.
    const std::wstring first(buf.data());
    const wchar_t* p = buf.data() + first.size() + 1;
    if (!*p) {
        out.push_back(narrow(first));
        return out;
    }
    while (*p) {
        const std::wstring name(p);
        out.push_back(narrow(first + L"\\" + name));
        p += name.size() + 1;
    }
    return out;
}

std::string get_text(HWND dlg, int id) {
    HWND c = GetDlgItem(dlg, id);
    const int n = GetWindowTextLengthW(c);
    std::wstring w(size_t(n) + 1, L'\0');
    GetWindowTextW(c, &w[0], n + 1);
    w.resize(size_t(n));
    return narrow(w);
}

void set_text(HWND dlg, int id, const std::string& text) { SetDlgItemTextW(dlg, id, widen(text).c_str()); }

std::string appdata_folder() {
    wchar_t buf[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA | CSIDL_FLAG_CREATE, nullptr, SHGFP_TYPE_CURRENT, buf)))
        return narrow(buf);
    return std::string();
}

std::string module_path() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(sizeof buf / sizeof buf[0]));
    return n ? narrow(std::wstring(buf, n)) : std::string();
}

} // namespace win32
} // namespace tn34xx
