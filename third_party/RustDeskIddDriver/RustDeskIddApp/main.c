#include <stdio.h>
#include <stdlib.h>
#include <conio.h>
#include <tchar.h>

#include "./IddController.h"

#pragma comment(lib, "kernel32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "winspool.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "odbc32.lib")
#pragma comment(lib, "odbccp32.lib")
#pragma comment(lib, "swdevice.lib")
#pragma comment(lib, "Cfgmgr32.lib")
#pragma comment(lib, "Setupapi.lib")
#pragma comment(lib, "Newdev.lib")

#define MAX_MONITOR_MODES 10


int prompt_input()
{
    printf("Press  key                  execute:\n");
    printf("       1. 'q'               1. quit\n");
    printf("       2. 'c'               2. create software device with lifetime \"SWDeviceLifetimeHandle\"\n");
    printf("       3. 'C'               3. create software device with lifetime \"SWDeviceLifetimeParentPresent\"\n");
    printf("       4. 'd'               4. destroy software device\n");
    printf("       5. 'i'               5. install or update driver\n");
    printf("       6. 'u'               6. uninstall driver\n");
    printf("       7. 'a'               7. plug in monitor\n");
    printf("       8. 'b'               8. plug out monitor\n");
    printf("       9. 'm'               9. update monitor modes\n");

    return _getch();
}

int __cdecl main(int argc, char* argv[])
{
    HSWDEVICE hSwDevice = NULL;
    BOOL bExit = FALSE;
    SW_DEVICE_LIFETIME lifetime = SWDeviceLifetimeHandle;

    DWORD width = 1920;
    DWORD height = 1080;
    DWORD sync = 60;

    UINT index = 0;

    TCHAR exePath[1024] = { 0, };
    (void)GetModuleFileName(NULL, exePath, sizeof(exePath)/sizeof(exePath[0]) - 1);
    *_tcsrchr(exePath, _T('\\')) = _T('\0');
    PTCHAR infPath = _T("RustDeskIddDriver\\RustDeskIddDriver.inf");
    TCHAR infFullPath[1024] = { 0, };
    _sntprintf_s(infFullPath, sizeof(infFullPath) / sizeof(infFullPath[0]), _TRUNCATE, _T("%s\\%s"), exePath, infPath);

    do
    {
        int key = prompt_input();
        BOOL rebootRequired = FALSE;
        switch (key)
        {
        case 'i':
            printf("Install or update driver begin\n");
            if (FALSE == InstallUpdate(infFullPath, &rebootRequired))
            {
                printf(GetLastMsg());
            }
            else
            {
                printf("Install or update driver done, reboot is %s required\n", (rebootRequired == TRUE ? "" : "not"));
            }
            break;
        case 'u':
            printf("Uninstall driver begin\n");
            if (FALSE == Uninstall(infFullPath, &rebootRequired))
            //if (FALSE == InstallUpdate(_T("D:\\projects\\windows\\IndirectDisplay\\x64\\Debug\\RustDeskIddDriver\\RustDeskIddDriver.inf"), &rebootRequired))
            {
                printf(GetLastMsg());
            }
            else
            {
                printf("Uninstall driver done, reboot is %s required\n", (rebootRequired == TRUE ? "" : "not"));
            }
            break;
        case 'c':
        case 'C':
            lifetime = key == 'c' ? SWDeviceLifetimeHandle : SWDeviceLifetimeParentPresent;
            printf("Create device begin\n");
            if (hSwDevice != NULL)
            {
                printf("Device created before\n");
                break;
            }
            if (FALSE == DeviceCreateWithLifetime(&lifetime, &hSwDevice))
            {
                printf(GetLastMsg());
                DeviceClose(hSwDevice);
                hSwDevice = NULL;
            }
            else
            {
                printf("Create device done\n");
            }
            break;
        case 'd':
            printf("Close device begin\n");
            DeviceClose(hSwDevice);
            hSwDevice = NULL;
            printf("Close device done\n");
            break;
        case 'a':
            printf("Plug in monitor begin, current index %u\n", index);
            if (FALSE == MonitorPlugIn(index, 0, 25))
            {
                printf(GetLastMsg());
            }
            else
            {
                printf("Plug in monitor done\n");

                MonitorMode modes[2] = { { 1920, 1080,  60 }, { 1024,  768,  60 }, };
                if (FALSE == MonitorModesUpdate(index, sizeof(modes)/sizeof(modes[0]), modes))
                {
                    printf(GetLastMsg());
                }

                index += 1;
            }
            break;
        case 'b':
            if (index == 0) {
                printf("No virtual monitors\n");
                break;
            }

            printf("Plug out monitor begin, current index %u\n", index - 1);
            if (FALSE == MonitorPlugOut(index - 1))
            {
                printf(GetLastMsg());
            }
            else
            {
                index -= 1;
                printf("Plug out monitor done\n");
            }
            break;
        case 'm':
            printf("Update monitor modes, current max index %u\n", index - 1);
            printf("Please select the monitor from 0 to %d\n", index - 1);
            UINT i = 0;
            scanf_s("%d%*c", &i);
            if (i >= index) {
                fprintf(stderr, "Please select index less equal to %d, your input is %d\n", index - 1, i);
            }
            else {
                printf("\nYou have selected %d monitor to update modes.\n", i);
                printf("Please add at lease one monitor mode, format: width, height, refresh frequency.\nThe max number of modes is %d.\nPress e to end input.\n", MAX_MONITOR_MODES);

                int input_ok = 0;
                int k = 0;
                MonitorMode modes[MAX_MONITOR_MODES] = { {0, 0, 0}, };
                while (1) {
                    char input[100];
                    if (fgets(input, sizeof(input), stdin) != NULL) {
                        if (input[0] == 'e') {
                            input_ok = 1;
                            break;
                        }
                        else {
                            sscanf_s(input, "%d,%d,%d", &modes[k].width, &modes[k].height, &modes[k].sync);
                        }
                    }
                    else {
                        fprintf(stderr, "Something is wrong while tring to read the input. Please try again\n");
                        break;
                    }
                    ++k;
                    if (k == sizeof(modes) / sizeof(modes[0])) {
                        input_ok = 1;
                        break;
                    }
                }

                if (input_ok == 1) {
                    if (k == 0) {
                        fprintf(stderr, "No monitor modes are added, skip.\n");
                    }
                    else {
                        if (FALSE == MonitorModesUpdate(i, k, modes))
                        {
                            printf(GetLastMsg());
                        }
                    }
                }
            }
            break;
        case 'q':
            bExit = TRUE;
            break;
        default:
            break;
        }

        printf("\n\n");
    }while (!bExit);

    if (hSwDevice)
    {
        SwDeviceClose(hSwDevice);
    }

    return 0;
}
