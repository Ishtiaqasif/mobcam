# Security policy

MobCam streams a camera across a network and installs a virtual camera that Windows loads into
a system service, so security reports are taken seriously. Thank you for reporting responsibly.

## Supported versions

Only the [latest release](https://github.com/Ishtiaqasif/mobcam/releases/latest) gets security
fixes. Update both the phone app and the Windows app before reporting.

| Version | Supported |
|---|---|
| 1.1.x | ✅ |
| 1.0.x | ❌ No Wi-Fi pairing or encryption: anyone on the same network can watch or control the camera. Update to 1.1. |

## Reporting a vulnerability

**Please don't open a public issue or discussion for security problems.** Report them privately:

1. Go to [**Security → Report a vulnerability**](https://github.com/Ishtiaqasif/mobcam/security/advisories/new).
2. Describe the problem, the versions affected (phone app and Windows app), and how to
   reproduce it. A proof of concept helps but isn't required.

Only the maintainers can see the report. There is no email address for reports; this keeps the
whole conversation, and the eventual fix, in one place on GitHub.

## What to expect

MobCam is maintained by one person in their spare time, so these are goals, not guarantees:

- **Acknowledgement** within 7 days.
- **Assessment** (confirmed or not, severity) within 14 days.
- **Fix** in a new release as soon as practical, usually within 90 days for confirmed issues.
  The advisory is published together with the fix, and you're credited unless you'd rather not
  be.

## In scope

- The Wi-Fi pairing, key exchange and encryption (protocol v2, [docs/protocol.md](docs/protocol.md)):
  watching the stream or controlling the phone without pairing, pairing without the user's
  confirmation on both devices, weaknesses in the encryption.
- The phone app's network server (TCP 5000) and discovery responder (UDP).
- The Windows app, `mobcam-cli`, the installer and the virtual camera DLL loaded by Windows
  Frame Server (for example privilege escalation or loading untrusted code).
- The release pipeline: tampered or unsigned artifacts, checksums that don't match.

## Out of scope

- Attacks that need someone who already controls the phone or the PC (root/admin, a malicious
  app with the same permissions, physical access to an unlocked device).
- USB connections: these require USB debugging, which already gives the PC full control of the
  phone.
- Denial of service on the local network (flooding the phone with connections or discovery
  packets).
- Vulnerabilities in dependencies with no demonstrated effect on MobCam (open a normal issue
  if one needs updating).
