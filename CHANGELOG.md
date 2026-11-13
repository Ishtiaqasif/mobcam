# Changelog

All notable changes to MobCam are documented here.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project
uses [Semantic Versioning](https://semver.org/).

## [Unreleased]
### Added
- **New version notice** in both apps, only if you allow it: the first time each app starts, it
  asks whether to check GitHub for new versions. If you say yes, it checks once a day and shows a
  notice with a link to the release page when there's a newer version (*Later* hides it until the
  next one). Nothing is downloaded or installed. Change it any time: *Check GitHub for new
  versions* in the Windows app's App section, or in *About* on the phone. The privacy statement
  describes exactly what is sent.

## [1.2.0] - 2026-11-13
### Added
- **Keep streaming in background** (phone, off by default): while a PC is connected, the camera
  keeps streaming with the screen off or another app open. An ongoing notification shows it's on
  and has a *Stop* button. If the PC goes away, it stops by itself after a minute. On Android 13+
  MobCam asks to show notifications when you turn it on.
- **About** in both apps (phone: below *Paired PCs*; Windows app: the App card): version, a
  short privacy summary, links to the project and the privacy statement, and the licences of the
  third-party software MobCam includes. The installer and the APK now ship
  `THIRD-PARTY-NOTICES.txt`.
- Privacy statement ([PRIVACY.md](PRIVACY.md)): what MobCam stores, where, and how to remove it.
  In short: no accounts, no analytics, and the video never leaves your phone and PC.

### Removed
- The phone app no longer requests the storage, "draw over other apps" and vibrate permissions.
  The app framework added them by default; MobCam never used them.

### Security
- The phone app now checks itself that a PC's session key is a valid P-256 point, instead of
  relying on Android's crypto provider to reject invalid ones.

## [1.1.0] - 2026-11-12
### Security
- **Wi-Fi pairing and encryption** (protocol v2). Others on the same Wi-Fi can no longer watch the
  camera or control the phone. The first time a PC connects over Wi-Fi, both screens show a
  6-digit code: tap *Allow* on the phone and *Codes match* on the PC (or `y` in `mobcam-cli`).
  Both sides must confirm, so a rogue device on the network can't pair itself with your PC either.
  After that the PC reconnects without asking. Video and commands are encrypted (AES-256-GCM) on
  Wi-Fi and USB alike; USB never asks.
- Paired devices are listed with *Forget*: **Paired PCs** on the phone, **Paired phones** in the
  Windows app.
- Phone and PC apps from before this change can't connect to each other; both tell you to update.
- Raw testing with `ffplay tcp://<phone>:5000` no longer works.

### Added
- Both apps show their version and the git commit they were built from (phone: bottom of the
  panel; Windows app: title bar and the App card), marked "modified" for builds with uncommitted
  changes, so you can tell exactly which build is running.

## [1.0.0] - 2026-11-12

First release.

### Phone app (Android 7+)
- Streams the camera as hardware-encoded H.264 at 480p, 720p or 1080p, 30 fps.
- Every lens is selectable: ultra-wide, wide, telephoto and front (multi-camera phones included).
- Works over a USB cable (adb) or the same Wi-Fi; the PC finds the phone automatically.
- Reports its orientation so the picture is always upright on the PC.
- Can be controlled from the PC (lens, quality).

### Windows app (Windows 11)
- **MobCam** virtual camera usable in Zoom, Teams, Meet, OBS, browsers and the Camera app.
- Desktop app with live preview, phone discovery, auto-reconnect, rotation and Fit/Fill framing.
- Runs in the tray, optional start with Windows.
- Installer that sets up the app and the virtual camera in one step.
- `mobcam-cli` command-line client.

[Unreleased]: https://github.com/Ishtiaqasif/mobcam/compare/v1.2.0...dev
[1.2.0]: https://github.com/Ishtiaqasif/mobcam/releases/tag/v1.2.0
[1.1.0]: https://github.com/Ishtiaqasif/mobcam/releases/tag/v1.1.0
[1.0.0]: https://github.com/Ishtiaqasif/mobcam/releases/tag/v1.0.0
