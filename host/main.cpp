#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

#include <atomic>
#include <thread>
#include <string>

#include "server.h"
#include "dda_capture.h"
#include "nvenc_encoder.h"
#include "input.h"
#include "virtual_display.h"

namespace {

std::atomic<bool> g_running{true};
FILE* g_logFile = nullptr;

// Log to both stdout and file
void log(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    printf("%s", buf);
    fflush(stdout);
    if (g_logFile) {
        fprintf(g_logFile, "%s", buf);
        fflush(g_logFile);
    }
}

// Find DXGI output index matching w×h (first match), or -1.
int findOutputBySize(uint32_t w, uint32_t h) {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return -1;
    int idx = 0;
    for (UINT a = 0; ; ++a) {
        Microsoft::WRL::ComPtr<IDXGIAdapter> ad;
        if (factory->EnumAdapters(a, &ad) == DXGI_ERROR_NOT_FOUND)
            break;
        for (UINT o = 0; ; ++o) {
            Microsoft::WRL::ComPtr<IDXGIOutput> out;
            if (ad->EnumOutputs(o, &out) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_OUTPUT_DESC od = {};
            out->GetDesc(&od);
            const uint32_t ow = od.DesktopCoordinates.right - od.DesktopCoordinates.left;
            const uint32_t oh = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;
            if (ow == w && oh == h)
                return idx;
            ++idx;
        }
    }
    return -1;
}

RECT outputDesktopRect(uint32_t outputIndex) {
    RECT rc = {};
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return rc;
    int idx = 0;
    for (UINT a = 0; ; ++a) {
        Microsoft::WRL::ComPtr<IDXGIAdapter> ad;
        if (factory->EnumAdapters(a, &ad) == DXGI_ERROR_NOT_FOUND)
            break;
        for (UINT o = 0; ; ++o) {
            Microsoft::WRL::ComPtr<IDXGIOutput> out;
            if (ad->EnumOutputs(o, &out) == DXGI_ERROR_NOT_FOUND)
                break;
            if (idx == (int)outputIndex) {
                DXGI_OUTPUT_DESC od = {};
                out->GetDesc(&od);
                return od.DesktopCoordinates;
            }
            ++idx;
        }
    }
    return rc;
}

// Run adb command; returns exit code, output captured to log.
int runAdb(const wchar_t* args) {
    const wchar_t* adbPaths[] = {
        L"D:\\dev\\OpenDisplay\\tools\\platform-tools\\adb.exe",
        L"adb.exe",
    };
    for (const auto* adb : adbPaths) {
        wchar_t cmd[1024];
        swprintf_s(cmd, L"\"%s\" %s", adb, args);

        // Create pipes to capture output
        SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
        HANDLE hRead, hWrite;
        CreatePipe(&hRead, &hWrite, &sa, 0);
        SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = hWrite;
        si.hStdError = hWrite;

        PROCESS_INFORMATION pi = {};
        if (CreateProcessW(nullptr, cmd, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(hWrite);
            WaitForSingleObject(pi.hProcess, 15000);

            char output[4096] = {};
            DWORD bytesRead;
            ReadFile(hRead, output, sizeof(output) - 1, &bytesRead, nullptr);

            DWORD exitCode = 1;
            GetExitCodeProcess(pi.hProcess, &exitCode);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            CloseHandle(hRead);

            if (bytesRead > 0) {
                // Convert to log
                log("[adb] %s\n", output);
            }
            return (int)exitCode;
        }
        CloseHandle(hRead);
        CloseHandle(hWrite);
    }
    log("[adb] adb.exe not found!\n");
    return -1;
}

// Setup adb reverse and launch app on phone
bool setupAdb(uint16_t port) {
    log("[*] Setting up adb reverse tcp:%u...\n", port);

    wchar_t args[128];
    swprintf_s(args, L"reverse tcp:%u tcp:%u", port, port);
    int rc = runAdb(args);
    if (rc != 0) {
        log("[!] adb reverse failed (code=%d). Is phone connected with USB debugging?\n", rc);
        return false;
    }
    log("[+] adb reverse OK\n");

    // Launch app with port parameter
    log("[*] Launching app on phone (port %u)...\n", port);
    wchar_t startArgs[256];
    swprintf_s(startArgs, L"shell am start -n com.opendisplay.app/.MainActivity --ei port %u", port);
    rc = runAdb(startArgs);
    if (rc == 0) {
        log("[+] App launched on phone\n");
    } else {
        log("[!] Could not auto-launch app (code=%d). Open it manually on phone.\n", rc);
    }

    return true;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    // Open log file next to exe (shared so control panel can read it)
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeDir(exePath);
    exeDir = exeDir.substr(0, exeDir.find_last_of(L'\\'));
    std::wstring logPath = exeDir + L"\\host.log";
    g_logFile = _wfsopen(logPath.c_str(), L"w", _SH_DENYNO);

    uint16_t port = OD_PORT;
    uint32_t qp = 18;
    int displayIdx = -1;
    bool useVirtual = true;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc)
            port = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--qp") && i + 1 < argc)
            qp = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--display") && i + 1 < argc)
            displayIdx = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-virtual"))
            useVirtual = false;
    }

    log("OpenDisplay host v1 (HEVC, port %u, qp %u)\n", port, qp);

    Server server;
    std::string err;
    // Try port, then increment if busy
    bool listening = false;
    for (uint16_t tryPort = port; tryPort < port + 10; tryPort++) {
        if (server.listenOn(tryPort, err)) {
            port = tryPort;
            listening = true;
            break;
        }
    }
    if (!listening) {
        log("[!] listen failed: %s\n", err.c_str());
        if (g_logFile) fclose(g_logFile);
        return 1;
    }
    log("[+] listening on port %u\n", port);

    setupAdb(port);

    log("[*] Waiting for phone connection...\n");
    if (!server.acceptClient(err)) {
        log("[!] accept failed: %s\n", err.c_str());
        log("[!] Is the app open on the phone? Check USB debugging.\n");
        if (g_logFile) fclose(g_logFile);
        return 1;
    }
    log("[+] Phone connected!\n");

    OdCaps caps = {};
    if (!server.readCaps(caps, err)) {
        log("[!] caps failed: %s\n", err.c_str());
        if (g_logFile) fclose(g_logFile);
        return 1;
    }
    log("[+] Caps: %ux%u @%u Hz, codec flags 0x%02x\n",
        caps.max_w, caps.max_h, caps.max_hz, caps.codec_flags);

    // v0 mode choice: prefer 1920x1080@120 if phone can take it, else clamp.
    uint32_t mw = caps.max_w ? caps.max_w : 1920;
    uint32_t mh = caps.max_h ? caps.max_h : 1080;
    uint32_t modeW = (mw >= 1920) ? 1920 : mw;
    uint32_t modeH = (mh >= 1080) ? 1080 : mh;
    uint32_t modeHz = (caps.max_hz >= 120) ? 120 : (caps.max_hz ? caps.max_hz : 60);
    // Even dimensions for NV12.
    modeW &= ~1u;
    modeH &= ~1u;

    VirtualDisplay vdisplay;
    if (useVirtual) {
        int prevCount = VirtualDisplay::countOutputs();
        log("[vd] outputs before plug-in: %d\n", prevCount);

        std::string verr;
        if (!vdisplay.open(verr)) {
            log("[vd] %s\n", verr.c_str());
            log("[vd] driver not installed — using existing display\n");
            useVirtual = false;
        } else {
            if (!vdisplay.plugIn(0, 2, verr)) {
                log("[vd] plugIn failed: %s\n", verr.c_str());
                useVirtual = false;
            }

            if (useVirtual) {
                std::vector<VirtualDisplay::Mode> modes = {
                    { modeW, modeH, modeHz },
                    { 1920, 1080, 60 },
                    { 1280, 720, 60 },
                };
                if (!vdisplay.updateModes(0, modes, verr)) {
                    log("[vd] updateModes failed: %s (non-fatal)\n", verr.c_str());
                }
                log("[vd] virtual monitor plugged in (connector 0, %ux%u@%u)\n",
                    modeW, modeH, modeHz);

                Sleep(2000);

                int newIdx = VirtualDisplay::findNewOutput(modeW, modeH, prevCount);
                if (newIdx >= 0) {
                    displayIdx = newIdx;
                    log("[vd] found virtual display at output index %d\n", newIdx);
                } else {
                    log("[vd] could not find virtual display output\n");
                }
            }
        }
    }

    if (displayIdx < 0) {
        displayIdx = findOutputBySize(modeW, modeH);
        if (displayIdx < 0) {
            displayIdx = 0;
        }
    }

    DdaCapture dda;
    std::wstring werr;
    if (!dda.init((uint32_t)displayIdx, werr)) {
        log("[!] dda init failed\n");
        if (g_logFile) fclose(g_logFile);
        return 1;
    }
    log("[+] capturing output %d: %ux%u @%uHz\n", displayIdx, dda.width(), dda.height(), dda.refreshRate());

    const uint32_t encW = dda.width() & ~1u;
    const uint32_t encH = dda.height() & ~1u;

    NvencEncoder enc;
    if (!enc.init(dda.device(), encW, encH, modeHz, qp, werr)) {
        char msg[512];
        wcstombs(msg, werr.c_str(), sizeof(msg));
        log("[!] nvenc init failed: %s\n", msg);
        if (g_logFile) fclose(g_logFile);
        return 1;
    }
    log("[+] nvenc ready: %ux%u @%u\n", encW, encH, modeHz);

    if (!server.sendHandshake(OD_CODEC_HEVC, (uint16_t)encW, (uint16_t)encH,
                              (uint16_t)modeHz, err)) {
        log("[!] handshake failed: %s\n", err.c_str());
        if (g_logFile) fclose(g_logFile);
        return 1;
    }
    log("[+] handshake sent, streaming...\n");

    InputMapper input;
    RECT rect = outputDesktopRect((uint32_t)displayIdx);
    input.setDisplayRect(rect);

    std::thread control([&] {
        std::string cerr;
        server.pumpControl(
            [&](const OdTouch& t) { input.onTouch(t); },
            [&](const OdKey& k) { input.onKey(k); },
            [&](const OdScroll& s) { input.onScroll(s); },
            [] { log("[*] control: client requested stop\n"); g_running = false; },
            cerr);
        if (!cerr.empty() && cerr != "recv() failed or closed")
            log("[!] control: %s\n", cerr.c_str());
        else if (cerr.empty())
            log("[*] control: pumpControl returned empty (thread exit)\n");
        g_running = false;
    });

    uint64_t frameIdx = 0;
    uint64_t encodedFrames = 0;
    uint64_t bytesSent = 0;
    ULONGLONG t0 = GetTickCount64();

    auto sink = [&](const uint8_t* data, size_t size, bool keyframe, bool config) {
        uint64_t pf = frameIdx & ((1ull << 62) - 1);
        if (config)
            pf |= OD_PACKET_FLAG_CONFIG;
        if (keyframe)
            pf |= OD_PACKET_FLAG_KEY_FRAME;
        std::string serr;
        if (!server.sendVideo(data, (uint32_t)size, pf, serr)) {
            g_running = false;
            return;
        }
        bytesSent += size;
        ++frameIdx;
    };

    int idleBudget = 0;
    uint64_t encodeTimeTotal = 0;
    uint64_t ddaTimeouts = 0;
    uint64_t framesInWindow = 0;
    ULONGLONG statWindowStart = GetTickCount64();
    while (g_running) {
        ID3D11Texture2D* tex = nullptr;
        std::string aerr;
        int r = dda.acquire(1000 / (modeHz * 2), &tex, werr);
        if (r < 0) {
            log("[!] dda acquire failed\n");
            break;
        }
        if (r == 0) {
            ++ddaTimeouts;
            if (++idleBudget > (int)(modeHz * 5)) {
                Sleep(33);
            }
            continue;
        }
        idleBudget = 0;

        std::wstring eerr;
        const bool forceIdr = (frameIdx == 0);
        ULONGLONG encStart = GetTickCount64();
        if (!enc.encode(tex, forceIdr, sink, eerr)) {
            char msg[512];
            wcstombs(msg, eerr.c_str(), sizeof(msg));
            log("[!] encode failed: %s\n", msg);
            break;
        }
        encodeTimeTotal += GetTickCount64() - encStart;
        dda.releaseFrame();
        ++encodedFrames;
        ++framesInWindow;

        ULONGLONG now = GetTickCount64();
        if (now - statWindowStart >= 1000) {
            const double sec = (now - t0) / 1000.0;
            const double winSec = (now - statWindowStart) / 1000.0;
            log("[stats] %llu frames, %.1f Mbps, fps=%.1f, enc_avg=%.2fms, dda_timeout=%llu\n",
                (unsigned long long)frameIdx,
                sec > 0 ? (bytesSent * 8.0 / sec / 1e6) : 0.0,
                winSec > 0 ? (double)framesInWindow / winSec : 0.0,
                encodedFrames > 0 ? (double)encodeTimeTotal / encodedFrames : 0.0,
                (unsigned long long)ddaTimeouts);
            statWindowStart = now;
            framesInWindow = 0;
        }
    }

    g_running = false;
    server.closeClient();
    if (control.joinable())
        control.join();
    log("[*] Stopped\n");
    if (g_logFile) fclose(g_logFile);
    return 0;
}
