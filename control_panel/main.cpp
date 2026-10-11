// OpenDisplay Control Panel v0.3beta — gray theme, all-in-one
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <stdarg.h>
#include <string>
#include <fstream>
#include <sstream>
#include <initguid.h>
#include "virtual_display.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdi32.lib")

// Stubs for log() required by virtual_display.cpp
void log(const char*, ...) {}

// Colors
#define COL_BG       RGB(32, 33, 36)    // dark gray background
#define COL_FG       RGB(232, 234, 237) // light text
#define COL_BTN      RGB(55, 57, 62)    // button bg
#define COL_BTN_HOT  RGB(70, 72, 78)    // button hover
#define COL_ACCENT   RGB(0, 122, 204)   // blue accent
#define COL_GREEN    RGB(52, 168, 83)   // green status
#define COL_RED      RGB(234, 67, 53)   // red status
#define COL_YELLOW   RGB(251, 188, 5)   // yellow status
#define COL_EDIT_BG  RGB(24, 25, 28)   // edit/log bg
#define COL_BORDER   RGB(60, 62, 68)   // border color

#define IDC_STATUS      1010
#define IDC_BTN_PLUG    1011
#define IDC_BTN_HOST    1012
#define IDC_CHK_AUTO    1013
#define IDC_BTN_QUIT    1014
#define IDC_LOG         1015
#define IDC_HOSTLOG     1016
#define IDC_TITLE       1017
#define IDC_LBL_STATUS  1018
#define IDC_LBL_LOG     1019
#define IDC_LBL_HOSTLOG 1020
#define IDC_DOT         1021
#define IDT_UPDATE      2001

static HINSTANCE g_hInst;
static bool g_monitorPlugged = false;
static bool g_hostRunning = false;
static VirtualDisplay g_vd;
static HWND g_hwnd;
static HWND g_hStatus, g_hBtnPlug, g_hBtnHost, g_hChkAuto, g_hLog, g_hHostLog;
static HWND g_hDot, g_hTitle, g_hLblStatus, g_hLblLog, g_hLblHostLog;
static HANDLE g_hostProcess = nullptr;

// Brushes / fonts
static HBRUSH g_bgBrush, g_btnBrush, g_editBrush, g_borderBrush;
static HFONT g_fontTitle, g_fontNormal, g_fontSmall, g_fontMono;

static void stopHost();
static void log(const wchar_t* fmt, ...) {
    wchar_t buf[512];
    va_list args;
    va_start(args, fmt);
    vswprintf_s(buf, fmt, args);
    va_end(args);

    int len = GetWindowTextLengthW(g_hLog);
    SendMessageW(g_hLog, EM_SETSEL, len, len);
    SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)buf);
    SendMessageW(g_hLog, EM_SCROLL, SB_LINEDOWN, 0);
}

static std::wstring getExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path);
    return dir.substr(0, dir.find_last_of(L'\\'));
}

static void updateHostLog() {
    std::wstring logPath = getExeDir() + L"\\..\\..\\host\\bin\\host.log";
    std::ifstream file(logPath);
    if (!file.is_open()) {
        logPath = L"D:\\dev\\OpenDisplay\\host\\bin\\host.log";
        file.open(logPath);
        if (!file.is_open()) return;
    }

    std::stringstream ss;
    ss << file.rdbuf();
    std::string content = ss.str();
    file.close();

    int wlen = MultiByteToWideChar(CP_UTF8, 0, content.c_str(), -1, nullptr, 0);
    std::wstring wcontent(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, content.c_str(), -1, &wcontent[0], wlen);
    wcontent.resize(wlen - 1);

    SetWindowTextW(g_hHostLog, wcontent.c_str());
    SendMessageW(g_hHostLog, EM_SETSEL, wcontent.size(), wcontent.size());
    SendMessageW(g_hHostLog, EM_SCROLLCARET, 0, 0);
}

static bool isDriverInstalled() {
    VirtualDisplay vd;
    std::string err;
    if (vd.open(err)) { vd.close(); return true; }
    return false;
}

static bool isAutostart() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    wchar_t buf[MAX_PATH] = {};
    DWORD size = sizeof(buf);
    LONG r = RegQueryValueExW(key, L"OpenDisplay", nullptr, nullptr, (LPBYTE)buf, &size);
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

static void setAutostart(bool enable) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;
    if (enable) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(path) + L"\"";
        RegSetValueExW(key, L"OpenDisplay", 0, REG_SZ,
            (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, L"OpenDisplay");
    }
    RegCloseKey(key);
}

static bool isProcessRunning(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) { found = true; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

static void updateStatus() {
    bool drv = isDriverInstalled();
    g_hostRunning = isProcessRunning(L"opendisplay-host.exe");

    wchar_t text[256];
    if (!drv) {
        swprintf_s(text, L"Driver NOT INSTALLED  |  Host: %s", g_hostRunning ? L"running" : L"stopped");
        SetTextColor(GetDC(g_hDot), COL_RED);
    } else if (g_monitorPlugged) {
        swprintf_s(text, L"Ready  |  Monitor: plugged  |  Host: %s", g_hostRunning ? L"running" : L"stopped");
    } else {
        swprintf_s(text, L"Driver OK  |  Monitor: unplugged  |  Host: %s", g_hostRunning ? L"running" : L"stopped");
    }
    SetWindowTextW(g_hStatus, text);

    // Update dot color
    COLORREF dotColor = !drv ? COL_RED : (g_hostRunning ? COL_GREEN : COL_YELLOW);
    HBRUSH newDot = CreateSolidBrush(dotColor);
    HBRUSH oldDot = (HBRUSH)SetClassLongPtrW(g_hDot, GCLP_HBRBACKGROUND, (LONG_PTR)newDot);
    if (oldDot) DeleteObject(oldDot);
    InvalidateRect(g_hDot, nullptr, TRUE);

    SetWindowTextW(g_hBtnPlug, g_monitorPlugged ? L"Unplug Monitor" : L"Plug Monitor");
    SetWindowTextW(g_hBtnHost, g_hostRunning ? L"Stop Host" : L"Start Host");
    SendMessageW(g_hChkAuto, BM_SETCHECK, isAutostart() ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void startHost() {
    stopHost();
    Sleep(500);

    std::wstring hostPath = getExeDir() + L"\\..\\..\\host\\bin\\opendisplay-host.exe";
    if (GetFileAttributesW(hostPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        hostPath = L"D:\\dev\\OpenDisplay\\host\\bin\\opendisplay-host.exe";
        if (GetFileAttributesW(hostPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            log(L"[!] host.exe not found!\n");
            return;
        }
    }

    log(L"[*] Starting host...\n");

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"open";
    sei.lpFile = hostPath.c_str();
    sei.nShow = SW_HIDE;
    if (ShellExecuteExW(&sei)) {
        g_hostProcess = sei.hProcess;
        log(L"[+] Host started (PID=%lu)\n", GetProcessId(sei.hProcess));
    } else {
        log(L"[!] Failed to start host (err=%lu)\n", GetLastError());
    }
}

static void stopHost() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = {};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"opendisplay-host.exe") == 0) {
                    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                    if (h) { TerminateProcess(h, 0); CloseHandle(h); }
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    log(L"[*] Host stopped\n");
}

static void toggleMonitor() {
    std::string err;
    if (!g_vd.isOpen()) {
        if (!g_vd.open(err)) {
            log(L"[!] Driver not installed: %hs\n", err.c_str());
            MessageBoxW(g_hwnd,
                L"Driver not installed.\nRun install.cmd as admin first.",
                L"OpenDisplay", MB_ICONWARNING);
            updateStatus();
            return;
        }
        log(L"[*] Driver interface opened\n");
    }

    if (g_monitorPlugged) {
        if (g_vd.plugOut(0, err)) {
            g_monitorPlugged = false;
            log(L"[*] Monitor unplugged\n");
            stopHost();
        } else {
            log(L"[!] Plug-out failed: %hs\n", err.c_str());
        }
    } else {
        if (g_vd.plugIn(0, 2, err)) {
            g_monitorPlugged = true;
            log(L"[*] Monitor plugged in\n");
        } else {
            log(L"[!] Plug-in failed (%hs), trying unplug first...\n", err.c_str());
            std::string e2;
            g_vd.plugOut(0, e2);
            Sleep(500);
            if (g_vd.plugIn(0, 2, err)) {
                g_monitorPlugged = true;
                log(L"[*] Monitor plugged in (after unplug)\n");
            } else {
                log(L"[!] Plug-in still failed: %hs\n", err.c_str());
            }
        }
        if (g_monitorPlugged && !isProcessRunning(L"opendisplay-host.exe")) {
            startHost();
        }
    }
    updateStatus();
}

static HBRUSH CreateBtnBrush() { return CreateSolidBrush(COL_BTN); }

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;

        // Fonts
        g_fontTitle = CreateFontW(28, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        g_fontNormal = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        g_fontSmall = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        g_fontMono = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_MODERN, L"Consolas");

        // Title
        g_hTitle = CreateWindowExW(0, L"STATIC", L"OpenDisplay",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            20, 16, 300, 36, hwnd, (HMENU)IDC_TITLE, g_hInst, nullptr);
        SendMessageW(g_hTitle, WM_SETFONT, (WPARAM)g_fontTitle, TRUE);

        // Version label
        HWND ver = CreateWindowExW(0, L"STATIC", L"v0.3beta",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            320, 22, 100, 20, hwnd, (HMENU)0, g_hInst, nullptr);
        SendMessageW(ver, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);
        SetTextColor(GetDC(ver), COL_FG);

        // Status dot (colored circle via static)
        g_hDot = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_BITMAP,
            20, 60, 12, 12, hwnd, (HMENU)IDC_DOT, g_hInst, nullptr);

        // Status text
        g_hStatus = CreateWindowExW(0, L"STATIC", L"Status: checking...",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            40, 56, 540, 20, hwnd, (HMENU)IDC_STATUS, g_hInst, nullptr);
        SendMessageW(g_hStatus, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);

        // Buttons
        g_hBtnPlug = CreateWindowExW(0, L"BUTTON", L"Plug Monitor",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
            20, 90, 180, 40, hwnd, (HMENU)IDC_BTN_PLUG, g_hInst, nullptr);
        g_hBtnHost = CreateWindowExW(0, L"BUTTON", L"Start Host",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
            215, 90, 180, 40, hwnd, (HMENU)IDC_BTN_HOST, g_hInst, nullptr);
        HWND btnQuit = CreateWindowExW(0, L"BUTTON", L"Quit",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
            410, 90, 170, 40, hwnd, (HMENU)IDC_BTN_QUIT, g_hInst, nullptr);
        SendMessageW(g_hBtnPlug, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
        SendMessageW(g_hBtnHost, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
        SendMessageW(btnQuit, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);

        // Autostart checkbox
        g_hChkAuto = CreateWindowExW(0, L"BUTTON", L"Start with Windows",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            20, 140, 220, 24, hwnd, (HMENU)IDC_CHK_AUTO, g_hInst, nullptr);
        SendMessageW(g_hChkAuto, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);

        // Log label
        g_hLblLog = CreateWindowExW(0, L"STATIC", L"Actions",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            20, 175, 100, 18, hwnd, (HMENU)IDC_LBL_LOG, g_hInst, nullptr);
        SendMessageW(g_hLblLog, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);

        // Action log
        g_hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            20, 195, 560, 70, hwnd, (HMENU)IDC_LOG, g_hInst, nullptr);
        SendMessageW(g_hLog, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);

        // Host log label
        g_hLblHostLog = CreateWindowExW(0, L"STATIC", L"Host Log",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            20, 275, 100, 18, hwnd, (HMENU)IDC_LBL_HOSTLOG, g_hInst, nullptr);
        SendMessageW(g_hLblHostLog, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);

        // Host log
        g_hHostLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            20, 295, 560, 160, hwnd, (HMENU)IDC_HOSTLOG, g_hInst, nullptr);
        SendMessageW(g_hHostLog, WM_SETFONT, (WPARAM)g_fontMono, TRUE);

        updateStatus();
        SetTimer(hwnd, IDT_UPDATE, 1000, nullptr);
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        HWND hCtrl = (HWND)lParam;

        if (hCtrl == g_hDot) {
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)GetStockObject(NULL_BRUSH);
        }

        SetTextColor(hdc, COL_FG);
        SetBkColor(hdc, COL_BG);
        return (LRESULT)g_bgBrush;
    }

    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, COL_FG);
        SetBkColor(hdc, COL_EDIT_BG);
        return (LRESULT)g_editBrush;
    }

    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, COL_FG);
        SetBkColor(hdc, COL_BTN);
        return (LRESULT)g_btnBrush;
    }

    case WM_TIMER:
        if (wParam == IDT_UPDATE) {
            updateStatus();
            updateHostLog();
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BTN_PLUG: toggleMonitor(); return 0;
        case IDC_BTN_HOST:
            if (g_hostRunning) stopHost(); else startHost();
            updateStatus();
            return 0;
        case IDC_CHK_AUTO:
            setAutostart(SendMessageW(g_hChkAuto, BM_GETCHECK, 0, 0) == BST_CHECKED);
            return 0;
        case IDC_BTN_QUIT:
            if (g_monitorPlugged) { std::string e; g_vd.plugOut(0, e); }
            stopHost();
            PostQuitMessage(0);
            return 0;
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, IDT_UPDATE);
        if (g_bgBrush) DeleteObject(g_bgBrush);
        if (g_btnBrush) DeleteObject(g_btnBrush);
        if (g_editBrush) DeleteObject(g_editBrush);
        if (g_fontTitle) DeleteObject(g_fontTitle);
        if (g_fontNormal) DeleteObject(g_fontNormal);
        if (g_fontSmall) DeleteObject(g_fontSmall);
        if (g_fontMono) DeleteObject(g_fontMono);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    g_hInst = hInst;

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"OpenDisplayControlPanel");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"OpenDisplay is already running", L"OpenDisplay", MB_ICONINFORMATION);
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    // Create brushes
    g_bgBrush = CreateSolidBrush(COL_BG);
    g_btnBrush = CreateSolidBrush(COL_BTN);
    g_editBrush = CreateSolidBrush(COL_EDIT_BG);
    g_borderBrush = CreateSolidBrush(COL_BORDER);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"OpenDisplayControlPanel";
    wc.hbrBackground = g_bgBrush;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    // Center window
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int winW = 600, winH = 480;
    int x = (screenW - winW) / 2;
    int y = (screenH - winH) / 2;

    g_hwnd = CreateWindowExW(0, L"OpenDisplayControlPanel", L"OpenDisplay",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        x, y, winW, winH,
        nullptr, nullptr, hInst, nullptr);

    // Remove default background, use dark
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (mutex) CloseHandle(mutex);
    return 0;
}
