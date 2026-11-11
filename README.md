# MobCam

Use an Android phone as a webcam on Windows 11, over a USB cable or the same Wi-Fi.
The phone streams hardware-encoded H.264 (up to 1080p30); the PC decodes it and feeds a
virtual camera named **MobCam** that Zoom, Teams, Meet, OBS and the Camera app can pick.

```
 phone (app/)                                    PC (pc/)
┌───────────────────────────┐   TCP :5000    ┌──────────────────────────────┐
│ Expo / React Native UI    │  raw H.264     │ MobCam.exe (WPF)  mobcam-cli │
│ Kotlin module:            │ ─────────────▶ │        └── mobcam-core.dll   │
│  Camera2 → MediaCodec     │ ◀───────────── │  decode, rotate, fit/fill    │
│  TCP server, discovery    │  text commands │          │ shared memory      │
└───────────────────────────┘  USB or Wi-Fi  │ mobcam-source.dll (Frame     │
                                             │  Server virtual camera)      │
                                             └──────────────────────────────┘
```

## Repository layout

| Path | What |
|---|---|
| [app/](app/) | Android app (Expo SDK 58, React Native). The camera/encoder/server lives in the local native module [app/modules/mobcam-stream/](app/modules/mobcam-stream/) (Kotlin). |
| [pc/core/](pc/core/) | C++ engine with a C API ([mobcam_core.h](pc/core/mobcam_core.h)): discovery, adb, decoding, picture processing, virtual camera control. Built as `mobcam-core.dll`. |
| [pc/gui/](pc/gui/) | Windows app `MobCam.exe` (WPF, .NET 10, Fluent theme) on top of `mobcam-core.dll`. |
| [pc/cli/](pc/cli/) | `mobcam-cli.exe`, command-line front end and the elevated installer helper. |
| [pc/source/](pc/source/) | `mobcam-source.dll`, the Media Foundation virtual camera loaded by Windows Frame Server. |
| [pc/common/](pc/common/) | Code shared by the PC binaries (shared-memory layout, NV12 scaling). |
| [docs/protocol.md](docs/protocol.md) | Phone ⇄ PC wire protocol (stream, metadata, commands, discovery). |
| [pc/installer/](pc/installer/) | Inno Setup script for the Windows installer. |
| [scripts/](scripts/) | Dev, release and icon scripts. |

## Install (end users)

- **PC (Windows 11):** run `MobCam-Setup-<version>.exe`. It installs to `C:\Program Files\MobCam`
  and registers the virtual camera; no .NET install needed.
- **Phone (Android 7+):** install `MobCam-<version>.apk` (allow installing from this source when
  asked). USB mode additionally needs **USB debugging** turned on in Developer options.

## Release builds

```powershell
npm run release       # both of the below
npm run release:pc    # dist\MobCam-Setup-<version>.exe  (needs: winget install JRSoftware.InnoSetup)
npm run release:phone # dist\MobCam-<version>.apk        (add -Install via the script to adb-install it)
```

Bump the version in `pc/gui/MobCam.csproj` (PC) and `app/app.json` (`version` and
`android.versionCode`, phone) before a release.

The APK is signed with `%USERPROFILE%\.mobcam\mobcam-release.jks`, created on the first release
build with its password in `keystore.properties` next to it. **Back both files up**: Android only
installs updates signed with the same key.

## Development

Prerequisites: Node.js, Android SDK (platform-tools), JDK 17 (`winget install Microsoft.OpenJDK.17`),
Visual Studio 2022 **Build Tools** with the C++ workload and Windows 11 SDK (no IDE needed),
.NET 10 SDK.

```powershell
npm run phone           # debug build of the phone app over USB + Metro
npm run phone:typecheck
npm run pc:run          # build the PC side into pc\build\bin and start MobCam.exe
npm run icons           # regenerate the app icons from scripts/make-icons.ps1
```

A development build of MobCam.exe offers to **install/update the virtual camera** (one UAC
prompt): it copies `mobcam-source.dll` to `C:\Program Files\MobCam`, because Frame Server can't
load it from a user folder. A debug and a release APK are signed with different keys, so
uninstall one before installing the other (`adb uninstall com.mobcam.app`).

## Using it

1. Open MobCam on the phone.
2. Start `MobCam.exe` on the PC. It connects over USB if the phone is plugged in, otherwise it
   finds the phone on Wi-Fi.
3. Choose **MobCam** as the camera in your video app.

From the PC app you can pick the phone lens (ultra-wide / wide / telephoto / front), the
quality (480p/720p/1080p), rotation and Fit/Fill framing. Closing the window keeps MobCam
running in the tray.

## Troubleshooting

- Virtual camera log: `C:\ProgramData\MobCam\mobcam-source.log`
- Wi-Fi: allow MobCam through the Windows firewall when asked; phone and PC must be on the same network.
- Native builds of React Native libraries can exceed Windows' 260-character path limit in deep
  folders; enable long paths or keep the repo near the drive root if you add such libraries.
