# Privacy

_Last updated: 2026-11-13. Applies to the MobCam phone app (Android) and the MobCam Windows app._

**In short:** MobCam has no accounts, no analytics, no ads and no servers. Your video goes from
your phone to your own PC, over the USB cable or your local network, and nowhere else. MobCam's
developers never see it, and neither does anyone else.

## What MobCam does with your camera

- The phone app uses the camera only while it is open and streaming. If you turn on *Keep
  streaming in background* (off by default), it keeps streaming to a connected PC with the screen
  off or another app open; an ongoing notification shows this and has a *Stop* button. The
  picture is encoded on the phone and sent straight to the PC you connected. It isn't recorded or
  saved, on the phone or on the PC.
- On the PC, the picture is shown in the MobCam window and passed to the **MobCam** virtual
  camera, so the video apps you choose (Zoom, Teams, a browser, ...) can use it. What those apps
  do with the video is covered by their own privacy policies.
- Over Wi-Fi, a phone only streams to a PC you've paired with by confirming a code on both
  devices. Video and commands are encrypted (AES-256-GCM) on Wi-Fi and USB alike, so others on
  the same network can't watch or control the camera.

## Network use

- The phone app opens a server on your local network (TCP port 5000) and answers MobCam
  discovery requests (UDP), so the PC can find it. The discovery reply contains the phone's
  model name (for example "Google Pixel 8"), which any device on the same network can ask for.
- Over USB, the Windows app uses `adb` (Android's own tool) to reach the phone through the cable.
- **Neither app connects to the internet.** There is no telemetry, crash reporting or
  advertising code. If an optional "new version available" check is added later
  ([#16](https://github.com/Ishtiaqasif/mobcam/issues/16)), it will only ask GitHub for the
  latest release number, can be turned off, and will be described here before it ships.

## What is stored, and where

Everything stays on your own devices.

| Where | What |
|---|---|
| Phone, Android Keystore | The phone's pairing key. |
| Phone, app storage | For each paired PC: its name, an ID derived from its public key, and the pairing date. Whether *Keep streaming in background* is on. |
| PC, `%AppData%\MobCam\settings.json` | Preferences: auto-connect, tray, rotation, Fit/Fill, connection mode, the last address you typed. |
| PC, registry `HKCU\Software\MobCam\PairedPhones` | For each paired phone: its name, public key and pairing date. |
| PC, Windows key store (per user) | The PC's pairing key, "MobCam PC identity v1". |
| PC, `%ProgramData%\MobCam\mobcam-source.log` | Technical log of the virtual camera (start/stop, formats, errors). No pictures. |

The phone sends the PC its model name, and the PC sends the phone its computer name, so each
side can show which device is paired.

## Removing your data

- **Forget a device:** *Paired PCs* on the phone, *Paired phones* in the Windows app.
- **Phone:** uninstalling the app (or *Clear storage* in Android's app settings) removes
  everything it stored.
- **PC:** uninstalling keeps your settings and pairings so a reinstall picks up where you left
  off. To remove them too, delete `%AppData%\MobCam`, `%ProgramData%\MobCam` and the registry key
  `HKCU\Software\MobCam`.

## Permissions (Android)

| Permission | Why |
|---|---|
| Camera | To stream the camera. |
| Internet, network and Wi-Fi state | To serve the stream on your local network and over the USB tunnel. Newer Android versions also list *local network access*, which Android grants for the same reason. |
| Wi-Fi multicast | To receive the PC's discovery broadcast. |
| Notifications | Only with *Keep streaming in background*: the notification that shows the camera is streaming, with its *Stop* button. Asked for when you turn the setting on (Android 13+). |
| Foreground service (camera) | Only with *Keep streaming in background*: lets Android keep the camera running while the app isn't on screen. |
| Prevent phone from sleeping | Only with *Keep streaming in background*: keeps the processor and Wi-Fi awake while streaming with the screen off. |

That is the complete list. MobCam doesn't ask for storage, location, contacts, microphone,
"draw over other apps" or any other permission.

## Children

MobCam collects no personal information from anyone, children included.

## Changes and contact

Changes to this statement are listed in [CHANGELOG.md](CHANGELOG.md) and visible in the
[history of this file](https://github.com/Ishtiaqasif/mobcam/commits/dev/PRIVACY.md). Questions:
[Discussions → Q&A](https://github.com/Ishtiaqasif/mobcam/discussions/categories/q-a). Security
problems: [SECURITY.md](SECURITY.md).
