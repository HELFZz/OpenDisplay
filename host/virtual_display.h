#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#include <string>
#include <vector>

// GUID_DEVINTERFACE_IDD_DRIVER_DEVICE from Driver.cpp
// {781EF630-72B2-11d2-B852-00C04EAF5272}
DEFINE_GUID(GUID_DEVINTERFACE_IDD_DRIVER_DEVICE,
    0x781EF630, 0x72B2, 0x11d2, 0xB8, 0x52, 0x00, 0xC0, 0x4E, 0xAF, 0x52, 0x72);

// IOCTL codes — must match Public.h in the driver.
#ifndef IOCTL_CHANGER_BASE
#define IOCTL_CHANGER_BASE FILE_DEVICE_CHANGER
#endif

#define IOCTL_IDD_PLUG_IN \
    CTL_CODE(IOCTL_CHANGER_BASE, 0x1001, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_IDD_PLUG_OUT \
    CTL_CODE(IOCTL_CHANGER_BASE, 0x1002, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_IDD_UPDATE_MONITOR_MODE \
    CTL_CODE(IOCTL_CHANGER_BASE, 0x1003, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

#pragma pack(push, 1)
struct CtlPlugInH {
    uint32_t ConnectorIndex;
    uint32_t MonitorEDID;
    GUID ContainerId;
};

struct CtlPlugOutH {
    uint32_t ConnectorIndex;
};

struct CtlMonitorModesH {
    uint32_t ConnectorIndex;
    uint32_t ModeCount;
    struct {
        uint32_t Width;
        uint32_t Height;
        uint32_t Sync;
    } Modes[1];
};
#pragma pack(pop)

// Talks to the OpenDisplayIdd driver via its device interface to plug in
// a virtual monitor and set its modes. Requires the driver to be installed.
class VirtualDisplay {
public:
    ~VirtualDisplay();

    bool open(std::string& err);
    void close();

    bool plugIn(uint32_t connectorIndex, uint32_t edidIndex, std::string& err);

    struct Mode { uint32_t width, height, hz; };
    bool updateModes(uint32_t connectorIndex, const std::vector<Mode>& modes, std::string& err);

    bool plugOut(uint32_t connectorIndex, std::string& err);

    bool isOpen() const { return m_handle != INVALID_HANDLE_VALUE; }

    // After plug-in, enumerate DXGI outputs to find the new virtual display.
    // Returns the DXGI output index, or -1.
    static int findNewOutput(uint32_t expectedW, uint32_t expectedH, int prevCount);
    static int countOutputs();

private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
};
