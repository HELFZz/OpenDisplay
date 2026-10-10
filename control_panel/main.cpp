// OpenDisplay Control Panel — Win32 GUI, all-in-one
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
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

#define IDC_STATUS      1010
#define IDC_BTN_PLUG    1011
#define IDC_BTN_HOST    1012
#define IDC_CHK_AUTO    1013
#define IDC_BTN_QUIT    1014
#define IDC_LOG         1015
#define IDC_HOSTLOG     1016
#define IDT_UPDATE      2001

static HINSTANCE g_hInst;
static bool g_monitorPlugged = false;
static bool g_hostRunning = false;
static VirtualDisplay g_vd;
static HWND g_hwnd;
static HWND g_hStatus, g_hBtnPlug, g_hBtnHost, g_hChkAuto, g_hLog, g_hHostLog;
static HANDLE g_hostProcess = nullptr;

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

// Read host.log and display in the host log window
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

    // Convert to wide
    int wlen = MultiByteToWideChar(CP_UTF8, 0, content.c_str(), -1, nullptr, 0);
    std::wstring wcontent(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, content.c_str(), -1, &wcontent[0], wlen);
    wcontent.resize(wlen - 1); // remove null

    SetWindowTextW(g_hHostLog, wcontent.c_str());
    // Scroll to bottom
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
    wchar_t text[256];
    bool drv = isDriverInstalled();
    g_hostRunning = isProcessRunning(L"opendisplay-host.exe");

    if (!drv) {
        swprintf_s(text, L"Driver: NOT INSTALLED | Host: %s", g_hostRunning ? L"running" : L"stopped");
    } else if (g_monitorPlugged) {
        swprintf_s(text, L"Driver: OK | Monitor: PLUGGED | Host: %s", g_hostRunning ? L"running" : L"stopped");
    } else {
        swprintf_s(text, L"Driver: OK | Monitor: unplugged | Host: %s", g_hostRunning ? L"running" : L"stopped");
    }
    SetWindowTextW(g_hStatus, text);
    SetWindowTextW(g_hBtnPlug, g_monitorPlugged ? L"Unplug Monitor" : L"Plug Monitor");
    SetWindowTextW(g_hBtnHost, g_hostRunning ? L"Stop Host" : L"Start Host");
    SendMessageW(g_hChkAuto, BM_SETCHECK, isAutostart() ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void startHost() {
    // Kill any existing host first
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
    sei.lpParameters = L"--no-virtual";
    sei.nShow = SW_HIDE;
    if (ShellExecuteExW(&sei)) {
        g_hostProcess = sei.hProcess;
        log(L"[+] Host started (PID=%lu)\n", GetProcessId(sei.hProcess));
    } else {
        log(L"[!] Failed to start host (err=%lu)\n", GetLastError());
    }
}

static void stopHost() {
    // Kill all host processes
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
        // Try plug-in; if fails (already exists), unplug first then retry
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

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        g_hStatus = CreateWindowExW(0, L"STATIC",
            L"Status: checking...", WS_CHILD | WS_VISIBLE | SS_LEFT,
            12, 12, 560, 20, hwnd, (HMENU)IDC_STATUS, g_hInst, nullptr);
        g_hBtnPlug = CreateWindowExW(0, L"BUTTON", L"Plug Monitor",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            12, 40, 180, 32, hwnd, (HMENU)IDC_BTN_PLUG, g_hInst, nullptr);
        g_hBtnHost = CreateWindowExW(0, L"BUTTON", L"Start Host",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            202, 40, 180, 32, hwnd, (HMENU)IDC_BTN_HOST, g_hInst, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Quit",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            392, 40, 180, 32, hwnd, (HMENU)IDC_BTN_QUIT, g_hInst, nullptr);
        g_hChkAuto = CreateWindowExW(0, L"BUTTON", L"Start with Windows",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            12, 80, 200, 24, hwnd, (HMENU)IDC_CHK_AUTO, g_hInst, nullptr);
        g_hLog = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            12, 110, 560, 80, hwnd, (HMENU)IDC_LOG, g_hInst, nullptr);
        CreateWindowExW(0, L"STATIC", L"Host log:",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            12, 195, 100, 16, hwnd, (HMENU)0, g_hInst, nullptr);
        g_hHostLog = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            12, 212, 560, 140, hwnd, (HMENU)IDC_HOSTLOG, g_hInst, nullptr);
        updateStatus();
        SetTimer(hwnd, IDT_UPDATE, 1000, nullptr);
        return 0;

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

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"OpenDisplayControlPanel";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, L"OpenDisplayControlPanel", L"OpenDisplay",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 600, 400,
        nullptr, nullptr, hInst, nullptr);

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
