#include <initguid.h>
#include "virtual_display.h"
#include "log.h"
#include <dxgi1_6.h>
#include <cfgmgr32.h>
#include <setupapi.h>
#include <cstdio>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "user32.lib")

VirtualDisplay::~VirtualDisplay() { close(); }

bool VirtualDisplay::open(std::string& err)
{
    HDEVINFO devInfo = SetupDiGetClassDevsW(
        &GUID_DEVINTERFACE_IDD_DRIVER_DEVICE, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        err = "SetupDiGetClassDevs failed (driver not installed?)";
        return false;
    }

    SP_DEVICE_INTERFACE_DATA ifData{};
    ifData.cbSize = sizeof(ifData);

    if (!SetupDiEnumDeviceInterfaces(devInfo, nullptr,
        &GUID_DEVINTERFACE_IDD_DRIVER_DEVICE, 0, &ifData)) {
        err = "SetupDiEnumDeviceInterfaces failed (driver not installed?)";
        SetupDiDestroyDeviceInfoList(devInfo);
        return false;
    }

    DWORD required = 0;
    SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, nullptr, 0, &required, nullptr);
    if (required == 0) {
        err = "SetupDiGetDeviceInterfaceDetailW size query failed";
        SetupDiDestroyDeviceInfoList(devInfo);
        return false;
    }

    std::vector<uint8_t> buf(required);
    auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

    if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, required, nullptr, nullptr)) {
        err = "SetupDiGetDeviceInterfaceDetailW failed";
        SetupDiDestroyDeviceInfoList(devInfo);
        return false;
    }

    SetupDiDestroyDeviceInfoList(devInfo);

    m_handle = CreateFileW(detail->DevicePath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (m_handle == INVALID_HANDLE_VALUE) {
        err = "CreateFile on driver interface failed (run as admin?)";
        return false;
    }

    return true;
}

void VirtualDisplay::close()
{
    if (m_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
    }
}

bool VirtualDisplay::plugIn(uint32_t connectorIndex, uint32_t edidIndex, std::string& err)
{
    CtlPlugInH in{};
    in.ConnectorIndex = connectorIndex;
    in.MonitorEDID = edidIndex;
    in.ContainerId = GUID{ 0x12345678, 0x1234, 0x1234,
                           {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0} };

    DWORD returned = 0;
    if (!DeviceIoControl(m_handle, IOCTL_IDD_PLUG_IN,
        &in, sizeof(in), nullptr, 0, &returned, nullptr)) {
        err = "IOCTL_IDD_PLUG_IN failed (err=" + std::to_string(GetLastError()) + ")";
        return false;
    }
    return true;
}

bool VirtualDisplay::updateModes(uint32_t connectorIndex,
    const std::vector<Mode>& modes, std::string& err)
{
    if (modes.empty()) { err = "no modes"; return false; }

    size_t bufSize = sizeof(CtlMonitorModesH) + (modes.size() - 1) * sizeof(CtlMonitorModesH::Modes);
    std::vector<uint8_t> buf(bufSize);
    auto* cmd = reinterpret_cast<CtlMonitorModesH*>(buf.data());
    cmd->ConnectorIndex = connectorIndex;
    cmd->ModeCount = (uint32_t)modes.size();

    for (size_t i = 0; i < modes.size(); i++) {
        cmd->Modes[i].Width  = modes[i].width;
        cmd->Modes[i].Height = modes[i].height;
        cmd->Modes[i].Sync   = modes[i].hz;
    }

    DWORD returned = 0;
    if (!DeviceIoControl(m_handle, IOCTL_IDD_UPDATE_MONITOR_MODE,
        buf.data(), (DWORD)bufSize, nullptr, 0, &returned, nullptr)) {
        err = "IOCTL_IDD_UPDATE_MONITOR_MODE failed (err=" + std::to_string(GetLastError()) + ")";
        return false;
    }
    return true;
}

bool VirtualDisplay::plugOut(uint32_t connectorIndex, std::string& err)
{
    CtlPlugOutH in{};
    in.ConnectorIndex = connectorIndex;
    DWORD returned = 0;
    if (!DeviceIoControl(m_handle, IOCTL_IDD_PLUG_OUT,
        &in, sizeof(in), nullptr, 0, &returned, nullptr)) {
        err = "IOCTL_IDD_PLUG_OUT failed (err=" + std::to_string(GetLastError()) + ")";
        return false;
    }
    return true;
}

int VirtualDisplay::countOutputs()
{
    int count = 0;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) return 0;

    IDXGIAdapter1* adapter = nullptr;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; a++) {
        IDXGIOutput* output = nullptr;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; o++) {
            count++;
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return count;
}

int VirtualDisplay::findNewOutput(uint32_t expectedW, uint32_t expectedH, int prevCount)
{
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) return -1;

    struct OutInfo { int idx; int w, h; uint32_t hz; bool attached; int left, top; };
    std::vector<OutInfo> outputs;
    int idx = 0;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; a++) {
        IDXGIOutput* output = nullptr;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; o++) {
            DXGI_OUTPUT_DESC desc{};
            output->GetDesc(&desc);
            uint32_t hz = 0;
            IDXGIOutput1* out1 = nullptr;
            if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&out1))) && out1) {
                DXGI_MODE_DESC md = {};
                md.Width = (UINT)(desc.DesktopCoordinates.right - desc.DesktopCoordinates.left);
                md.Height = (UINT)(desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top);
                md.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                DXGI_MODE_DESC matched = {};
                if (SUCCEEDED(out1->FindClosestMatchingMode(&md, &matched, nullptr)) &&
                    matched.RefreshRate.Denominator)
                    hz = matched.RefreshRate.Numerator / matched.RefreshRate.Denominator;
                out1->Release();
            }
            outputs.push_back({ idx,
                (int)(desc.DesktopCoordinates.right - desc.DesktopCoordinates.left),
                (int)(desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top),
                hz, desc.AttachedToDesktop != FALSE,
                desc.DesktopCoordinates.left, desc.DesktopCoordinates.top });
            idx++;
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();

    // Log all outputs for debugging
    for (auto& o : outputs) {
        log("[vd] output %d: %dx%d @%uHz attached=%d pos=(%d,%d)\n",
            o.idx, o.w, o.h, o.hz, (int)o.attached, o.left, o.top);
    }

    log("[vd] GetDisplayConfig-free: %d outputs\n", (int)outputs.size());

    // Strategy 0: if count increased, the new output is at index prevCount
    if (prevCount >= 0 && (int)outputs.size() > prevCount) {
        for (auto& o : outputs) {
            if (o.idx == prevCount) {
                log("[vd] new output after plug-in: %d (%dx%d @%uHz pos=(%d,%d))\n",
                    o.idx, o.w, o.h, o.hz, o.left, o.top);
                return o.idx;
            }
        }
    }

    // Strategy 1: find output that is NOT at (0,0) — secondary/virtual display
    int matchW = -1;
    for (auto& o : outputs) {
        if (expectedW > 0 && (uint32_t)o.w == expectedW && (uint32_t)o.h == expectedH) {
            if (matchW < 0) matchW = o.idx;
            // Prefer non-primary (not at 0,0)
            if (o.left != 0 || o.top != 0) {
                log("[vd] found secondary at output %d\n", o.idx);
                return o.idx;
            }
        }
    }
    if (matchW >= 0) {
        log("[vd] only primary match at output %d\n", matchW);
        return matchW;
    }

    // Strategy 2: find any non-primary output
    for (auto& o : outputs) {
        if (o.left != 0 || o.top != 0) {
            log("[vd] found non-primary at output %d\n", o.idx);
            return o.idx;
        }
    }

    return -1;
}

bool VirtualDisplay::enableConnectedDisplays(std::string& err)
{
    // GetDisplayConfig is not exported from user32.dll on this Windows build,
    // but SetDisplayConfig IS. Use SDC_TOPOLOGY_EXTEND to enable all connected displays.
    typedef LONG (WINAPI *SetDisplayConfig_t)(UINT32, const void*, UINT32, const void*, UINT32);
    static HMODULE u32 = LoadLibraryW(L"user32.dll");
    if (!u32) { err = "user32 load failed"; return false; }
    auto SetDC_ = (SetDisplayConfig_t)GetProcAddress(u32, "SetDisplayConfig");
    if (!SetDC_) { err = "SetDisplayConfig not found"; return false; }

    // SDC_TOPOLOGY_EXTEND | SDC_APPLY = 0x4 | 0x80 = 0x84
    // Do NOT use SDC_SAVE_TO_DATABASE — it persists bad topology across reboots
    LONG rc = SetDC_(0, nullptr, 0, nullptr, 0x84);
    log("[vd] SetDisplayConfig(TOPOLOGY_EXTEND) = %ld\n", rc);
    if (rc != ERROR_SUCCESS) {
        err = "SetDisplayConfig failed: " + std::to_string(rc);
        return false;
    }
    log("[vd] displays enabled\n");
    return true;
}
