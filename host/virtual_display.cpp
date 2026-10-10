#include <initguid.h>
#include "virtual_display.h"
#include <dxgi1_6.h>
#include <cfgmgr32.h>
#include <setupapi.h>
#include <cstdio>

#pragma comment(lib, "setupapi.lib")

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

    int idx = 0;
    int found = -1;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; a++) {
        IDXGIOutput* output = nullptr;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; o++) {
            DXGI_OUTPUT_DESC desc{};
            output->GetDesc(&desc);
            int w = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
            int h = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;

            if (expectedW > 0 && (uint32_t)w == expectedW && (uint32_t)h == expectedH) {
                found = idx;
            }
            idx++;
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();

    if (found < 0 && idx > prevCount) found = idx - 1;
    return found;
}
