# Changelog

All notable changes to MobCam are documented here.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project
uses [Semantic Versioning](https://semver.org/).

## [Unreleased]

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

[Unreleased]: https://github.com/Ishtiaqasif/mobcam/compare/v1.0.0...dev
[1.0.0]: https://github.com/Ishtiaqasif/mobcam/releases/tag/v1.0.0
