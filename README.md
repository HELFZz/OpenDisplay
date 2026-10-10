# OpenDisplay

**Turn your Android phone into a second monitor for Windows — open source.**

OpenDisplay is a complete virtual-display stack:

```
┌─────────────┐   USB (adb reverse)   ┌──────────────┐
│   Windows    │ ◄──────────────────► │ Android phone │
│  Host + PC  │   HEVC over TCP 27190 │   Receiver    │
└─────────────┘                        └──────────────┘
```

The host captures a virtual monitor with DXGI Desktop Duplication, encodes it with NVIDIA NVENC (HEVC, low-latency CQP), and streams it over a local TCP tunnel to an Android app that decodes with MediaCodec and renders fullscreen. Touch input on the phone is mapped back to absolute mouse events on Windows.

## Features

- **True virtual display** via UMDF IddCx indirect-display driver (appears as a real monitor in Windows Display Settings)
- **Hardware HEVC encoding** with NVENC (CQP, ultra-low-latency tuning)
- **Cross-adapter GPU copy** — works even when the virtual monitor is not on the NVIDIA adapter
- **Real cursor overlay** drawn from the live Windows cursor (position, shape, alpha)
- **Touch → absolute mouse** with configurable scaling
- **1080p @ up to 120 Hz** (tested: 1920×1080 @ 120 Hz, ~10–20 Mbps)
- **All-in-one control panel** — plug/unplug monitor, start/stop host, autostart
- **No root required** on the phone; uses `adb reverse` for the transport

## Components

| Component | Path | Description |
|-----------|------|-------------|
| IddCx driver | `driver/OpenDisplayIdd/` | UMDF virtual monitor driver (based on RustDeskIddDriver, MS-PL) |
| Host | `host/` | DXGI DDA capture → NVENC → TCP server → SendInput |
| Control panel | `control_panel/` | Win32 GUI: plug, stream, autostart |
| Android app | `android/` | MediaCodec HEVC decoder + touch input |
| Protocol | `proto/opendisplay_proto.h` | Shared wire format (handshake, video packets, input) |

## Requirements

- Windows 10/11 x64
- NVIDIA GPU with NVENC (Kepler or newer)
- Visual Studio 2022 + Windows Driver Kit (WDK)
- Android 8.0+ device with USB debugging
- `adb` (Android platform-tools)

## Build

### Driver

```
cd driver\OpenDisplayIdd
build.cmd
```

Install (requires test signing):

```
bcdedit /set testsigning on
:: reboot
install.cmd
```

### Host

```
cd host
build.cmd
```

### Control panel

```
cd control_panel
build.cmd
```

### Android app

```
cd android
gradlew assembleDebug
adb install app\build\outputs\apk\debug\app-debug.apk
```

## Usage

1. Install the driver (see above) — a new display appears in Windows Display Settings.
2. Connect the phone via USB, enable USB debugging.
3. Run `opendisplay-control.exe` (or `opendisplay-host.exe` directly).
4. The host sets up `adb reverse`, launches the app, and starts streaming.
5. The phone shows the virtual desktop. Touch = mouse.

Tap and hold on the phone screen to open settings (FPS, quality, host IP, port).

## Protocol (v1)

- **Transport:** TCP on `127.0.0.1:27190` (tunneled via `adb reverse`)
- **Handshake:** 16-byte hello + capabilities (width, height, hz, codec flags)
- **Video:** HEVC Annex-B, 12-byte header (frame size, PTS, flags for keyframe/config)
- **Input:** touch (absolute), key, scroll packets

See `proto/opendisplay_proto.h` for the exact layout.

## Credits & licenses

- **IddCx driver** derived from [RustDeskIddDriver](https://github.com/rustdesk/RustDeskIddDriver) (MS-PL)
- **IddCx sample** from [Windows-driver-samples](https://github.com/microsoft/Windows-driver-samples) (MIT)
- **NVENC headers** from [nv-codec-headers](https://github.com/FFmpeg/nv-codec-headers) v12.1 (BSD-3-Clause)
- Touch-to-mouse mapping inspired by [scrcpy](https://github.com/Genymobile/scrcpy) (Apache-2.0)

## Status

**v0.1-beta** — experimental. Works end-to-end on NVIDIA + Android. Known limitations:

- Requires NVIDIA GPU (NVENC)
- Requires test signing for the driver
- USB/adb only (no wireless yet)
- Cursor overlay is software-composited (minor CPU cost)

## License

TBD — pick a license before public redistribution.
