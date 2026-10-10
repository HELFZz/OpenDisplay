# OpenDisplay — статус проекта (обновить при выходе/входе)

## Идея
Open-source: телефон/планшет как настоящий второй монитор Windows через ADB (USB).
- Настоящий виртуальный драйвер (IddCx/UMDF), не зеркало.
- Визуально lossless, до 120 Гц, авто-подбор разрешения под декодер телефона.
- Тач-ввод с телефона → SendInput на ПК.
- Транспорт: `adb reverse tcp:PORT tcp:PORT`, raw TCP сокет, TCP_NODELAY.
- GPU: NVIDIA RTX 3050 Laptop → NVENC H.265 (fallback H.264).
- Телефон: Xiaomi 14T Pro (Media 9300+, HEVC decode 1080p120 ок), Android, USB debugging включён (однажды unauthorized — попросить юзера принять RSA-диалог при подключении).
- Юзер просил: качать всё на D:, НЕ юзать adb shell без таймаута (зависает если RSA не принят), ставить APK через adb install. Отладка отключена юзером — телефон не подключён, понадобится на этапах тестирования.

## Окружение (проверено)
- git 2.55, Java 21 (Temurin), winget 1.29.
- VS 2022 Build Tools: **D:\VS2022** (MSBuild + MSVC 14.44.35207).
- WDK 10.0.26100.6584 установлен: **C:\Program Files (x86)\Windows Kits\10**.
- NuGet-пакеты WDK/SDK 10.0.28000.2526 распакованы: **driver/packages/** (WDKContentRoot=NuGet `c\`, задачи 17.0 скопированы рядом с Microsoft.Kits.Logger — фикс MSB4062).
- **Сборка драйвера работает БЕЗ админа** через наш тулсет (см. «Тулсет» ниже). Админ нужен только для установки (pnputil/devgen) и testsigning.
- Android SDK: **D:\dev\OpenDisplay\tools\android** (cmdline-tools, platform-35, build-tools 35.0.0, platform-tools). Licenses приняты.
- Gradle 8.11: **D:\dev\OpenDisplay\tools\gradle-8.11**. JAVA_HOME=C:\Program Files\Java\jdk-21.0.10.
- Сборка APK: `$env:JAVA_HOME=...; $env:ANDROID_HOME=...; gradle assembleDebug --no-daemon --project-dir D:\dev\OpenDisplay\android`
- adb: **D:\dev\OpenDisplay\tools\platform-tools\adb.exe**.
- NVIDIA driver 610.62, NVENC доступен.
- dotnet отсутствует — nuget restore через tools\nuget.exe.

## Тулсет WindowsUserModeDriver10.0 (решено, работает)
VSIX WDK-тулсета не установлен (404). Свой тулсет: **tools/msbuild-vc/v170/Platforms/x64/PlatformToolsets/WindowsUserModeDriver10.0/** (Toolset.props + Toolset.targets).
- props: WDKContentRoot из Wow6432Node KitsRoot10 (fallback Program Files), EnsureTrailingSlash; **WDKBuildFolder автодетект** (10.0.28000.0 → 10.0.26100.0 по Exists WindowsDriver.Default.props) — работает и для NuGet-пакета, и для installed WDK; DDKPlatform=x64 ДО импорта WDK.props; импорты: Microsoft.Cpp.MSVC.Toolset.x64.props → DesignTime UAP.props (сам импортирует Microsoft.Cpp.WindowsSDK.props — НЕ дублировать!) → WindowsDriver.Default.props → WDK.x64.*.props. UAP.props в DesignTime НЕ импортировать (MSB4011).
- targets: Microsoft.CppCommon.targets → Microsoft.Cpp.WindowsSDK.targets → WindowsDriver.Common.targets.
- **Критично: 64-битный MSBuild** `D:\VS2022\MSBuild\Current\Bin\amd64\MSBuild.exe` (32-бит падает на x86 InfVerif.dll, в WDK только x64/arm64).
- **MSB4062 (ValidateNTTargetVersion)**: в installed WDK Microsoft.Kits.Logger.dll лежит в bin\10.0.26100.0\{x86,x64}, НЕ рядом с задачами → установить без админа нельзя. Фикс: собирать через NuGet-пакет (driver/packages, Logger рядом с задачами); junction `third_party/Windows-driver-samples/packages` → `driver/packages` для сэмплов. В NuGet c\build\10.0.28000.0\bin\ скопированы задачи 17.0 (VisualStudioVersion=17.0).
- Всегда: `-p:Driver_SpectreMitigation=false -p:SpectreMitigation=false` (Spectre-либ нет) `-p:RunCodeAnalysis=false`; кавычки с trailing backslash в VCTargetsPath ломают MSB1008.
- driver/Directory.Build.props — импорт NuGet WDK/SDK props (включён, работает).
- Сэмпл пропатчен: EnablePREfast=false + TreatWarningAsError=false в ItemDefinitionGroups (глобальные /p: не переопределяют metadata).

## Драйвер driver/OpenDisplayIdd/ ✅ СОБИРАЕТСЯ И ПОДПИСЫВАЕТСЯ
- OpenDisplayIdd.vcxproj (x64 only, GUID {63428ED0-2FF5-4D88-A151-5277A5CDE669}, UMDF 2.25, IddCx 1.4/1.6 defines, WPP, без Spectre/PREfast).
- OpenDisplayIdd.inf: Root\OpenDisplayIdd, Display class, UpperFilters IndirectKmd, DeviceGroupId OpenDisplayIddGroup, UmdfExtensions IddCx0102.
- Mode-листы в Driver.cpp: 1920x1080@120/60, 1280x720@120/60, до 4K@120 (пока статичные; dynamic modes через IOCTL — следующий шаг).
- build.cmd: amd64 MSBuild + VCTargetsPath + Spectre off.
- Результат сборки: `x64\Release\OpenDisplayIdd\` (OpenDisplayIdd.dll + .inf + .cat, test-signed WDK-ключом, Errors: None).
- install.cmd/uninstall.cmd: pnputil /add-driver + devgen /add /bus Root (devgen: `driver/packages/.../c/tools/10.0.28000.0/x64/devgen.exe`). **Не запускать — требует админа + testsigning + ребут.**

## Структура проекта
D:\dev\OpenDisplay\
- driver/OpenDisplayIdd — форк RustDeskIddDriver ✅ собирается (build.cmd, install.cmd, uninstall.cmd)
- host/ — C++: DXGI DDA + NVENC + socket + SendInput ✅ собирается (build.cmd → bin/opendisplay-host.exe)
  - nvenc_encoder.h/.cpp — NVENC H.265 D3D11 (BGRA→NV12 через VideoProcessorBlt, ULL preset P4, CQP qp=12)
  - dda_capture.h/.cpp — DXGI OutputDuplication
  - server.h/.cpp — TCP-сервер, протокол (12-байт видео-заголовок, control framing)
  - input.h/.cpp — тач/клавиши → SendInput (абсолютные координаты через виртуальный рабочий стол)
  - main.cpp — оркестрация: adb reverse → accept → caps → handshake → capture→encode→send
- android/ — Kotlin APK ✅ исходники написаны (MainActivity.kt, Proto.kt, Manifest, Gradle build files)
  - Не собран — нужен Android SDK (cmdline-tools, platform-35, build-tools)
- proto/opendisplay_proto.h — общий протокол (handshake, video packet, control: caps/touch/key/scroll)
- scripts/ — install-driver, setup, enable-testsign (пусто)
- third_party/RustDeskIddDriver — клон (MS-PL) ✅
- third_party/Windows-driver-samples — клон (MIT), эталон IddSampleDriver ✅
- third_party/nv-codec-headers — клон FFmpeg (BSD, nvEncodeAPI.h) ✅
- tools/msbuild-vc — наш тулсет ✅
- tools/platform-tools — adb ✅
- tools/android — пусто, качнуть cmdline-tools
- driver/packages — NuGet WDK/SDK 10.0.28000.2526 ✅

## Ключевые находки по коду
- RustDeskIddDriver: IOCTL plug-in/out мониторов + IddCxMonitorUpdateModes. Public.h: CtlPlugIn{ConnectorIndex, MonitorEDID, ContainerId}, CtlMonitorModes{ConnectorIndex, ModeCount, Modes[]}. Device interface GUID {781EF630-72B2-11d2-B852-00C04EAF5272}. \Device\RustDeskIddDriver. Макс 10 мониторов. Наш путь: плагиним монитор, апдейтим modes под capabilities телефона.
- IddController.c в RustDeskIddApp: установка через UpdateDriverForPlugAndPlayDevices, SwDeviceCreate — переиспользуем для host.exe.
- FillSignalInfo: pixelRate = VSync*W*H, vSyncFreq — ок для 120.
- scrcpy (Apache-2.0): 12-байт заголовок (pts_flags u64be + size u32be), KEY_LATENCY=1, REPEAT_PREVIOUS_FRAME_AFTER, low-delay decode.
- NVENC sample (nvEncDXGIOutputDuplicationSample): DDA → NvEncoderD3D11, PRESET_LOW_LATENCY_HP, ARGB→NV12 через видеопроцессор. nv-codec-headers → динамическая загрузка nvEncodeAPI64.dll.
- MSBuild-диагностика: Fusion-логи HKCU\Software\Microsoft\Fusion (ForceLog/LogPath) для MSB4062; ReflectionTypeLoadException.LoaderExceptions в PowerShell для резолва зависимостей задач.

## Архитектура (финальная)
```
Windows:
  OpenDisplayIdd.sys (UMDF, форк RustDeskIddDriver)
     - плагин 1 монитор, dynamic modes через IOCTL (резолюции+Гц из handshake)
  opendisplay-host.exe
     - запускает adb reverse, слушает TCP
     - IOCTL: plug-in монитора, set modes
     - ChangeDisplaySettingsEx / SetDisplayConfig → форсим режим
     - DXGI DDA на виртуальном мониторе (OutputDuplication по output name \\.\DISPLAYN)
     - NVENC H.265 low-latency, GOP ~1-2s, VBR/constqp qp=8..14 (визуально lossless), битрейт лимит 400 Мбит
     - SendInput для тача
     - Protocol: handshake (phone → capabilities: w/h/hz list, codec support), host → chosen mode + codec + SPS/PPS; затем видеопакеты scrcpy-style
Android:
  app: SurfaceView/MediaCodec HEVC decode low-latency → onTouchEvent → control channel (reverse: телефон коннектится к host-серверу, один сокет: видео PC→phone, control phone→PC)
Транспорт: adb reverse tcp:27183 tcp:27183
```

## Этапы (todo)
1. ✅ Окружение (VS/WDK/adb/NuGet/Android SDK/Gradle).
2. ✅ Драйвер собирается и test-sign'ится. Осталось: установка/ребут (требует юзера: админ + bcdedit testsigning on + reboot).
3. ✅ Host v0 собирается: `host/build.cmd` → `host/bin/opendisplay-host.exe`.
4. ✅ Host plug-in виртуального монитора: `host/virtual_display.{h,cpp}` — SetupDiGetClassDevs + DeviceIoControl → IOCTL_IDD_PLUG_IN/UPDATE_MONITOR_MODE/PLUG_OUT, findNewOutput по разрешению. Флаг `--no-virtual` для захвата существующего монитора.
5. ✅ Android v0 собирается: APK с настройками FPS (15-240), Quality/QP (0-51), Host IP. Long-press → диалог настроек. `app-debug.apk` (2.4 МБ).
6. ✅ Control Panel: `control_panel/bin/opendisplay-control.exe` — tray-иконка, Plug/Unplug монитор, Autostart (HKCU Run), Start Host.
7. End-to-end тест: нужен телефон (USB debugging + RSA) + установленный драйвер.
8. README, финальная полировка.

## Лицензии (для репо)
- RustDeskIddDriver: Microsoft Public License (MS-PL) — форк ок, сохранить LICENSE.
- Windows-driver-samples: MIT.
- scrcpy: Apache-2.0 — заимствовать протокол/идеи, указать attribution.
- nv-codec-headers: BSD-3-Clause (FFmpeg).
- Наш код: Apache-2.0 предположительно (уточнить у юзера).

## Открытые вопросы юзеру
- Лицензия репозитория (Apache-2.0 / MIT / GPL)?
- Публиковать APK или только исходники?
