# Contributing to MobCam

Thanks for your interest! Bug reports, ideas and pull requests are all welcome.

## Reporting bugs

Open an issue with the **Bug report** template. Please include your phone model and Android
version, your Windows build, whether you use USB or Wi-Fi, and, for camera problems, the log at
`C:\ProgramData\MobCam\mobcam-source.log`.

## Development setup

Everything builds from the command line on Windows 11; see
[Development](README.md#development) in the README for prerequisites and commands.

```powershell
npm run phone           # debug build of the phone app on a USB-connected phone
npm run pc:run          # build and start the Windows app
npm run phone:typecheck
```

## Pull requests

- Keep each pull request focused on one change and describe how you tested it
  (phone model, USB/Wi-Fi, which video app).
- The phone and the PC talk over the protocol in [docs/protocol.md](docs/protocol.md). If you
  change one side, update the other side and the document in the same pull request, and keep
  older peers working where you can.
- CI must pass (PC build, phone typecheck and Android build).
- Match the style of the surrounding code; comments explain *why*, not *what*.

By contributing you agree that your contributions are licensed under the [MIT License](LICENSE).
