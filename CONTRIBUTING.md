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

## Questions and ideas

Use [Discussions](https://github.com/Ishtiaqasif/mobcam/discussions): **Q&A** for help,
**Ideas** for suggestions (upvote the ones you want). Issues are for planned work and confirmed
bugs.

## How work is tracked

- Every planned change is an **issue**; the pinned
  [🗺️ Roadmap](https://github.com/Ishtiaqasif/mobcam/issues/21) lists them all, and big items
  are split into sub-issues.
- **Milestones** (`v1.1 Safe to share`, ...) group issues into releases.
- **Labels**: `area: *` (phone, pc, protocol, ci, docs), `size: S/M/L`, `priority: *`, and
  `needs-decision` for items whose open questions must be answered first.
  `good first issue` marks easy starting points.
- The [**project board**](https://github.com/users/Ishtiaqasif/projects/4) shows the status of each issue (Todo → In progress → In review → Done).

## Branches

- `dev` is the **default branch** and where development happens. Branch off `dev`
  (`feature/<issue>-<topic>`, `fix/<issue>-<topic>`) and open pull requests against `dev`.
- `main` holds released code. It is protected: changes land only through pull requests with a
  passing CI, and every release is tagged from it (`v1.2.3`).
- To release, open a pull request from `dev` to `main`, merge it with a merge commit, then tag
  and publish (see [Releasing](README.md#releasing)).

Put `Closes #<issue>` in the pull request description: merging into `dev` closes the issue and
moves it to **Done** on the board.

## Pull requests

- Keep each pull request focused on one change and describe how you tested it
  (phone model, USB/Wi-Fi, which video app).
- The phone and the PC talk over the protocol in [docs/protocol.md](docs/protocol.md). If you
  change one side, update the other side and the document in the same pull request, and keep
  older peers working where you can.
- CI must pass (PC build, phone typecheck, third-party notices and Android build).
- New dependencies need a licence compatible with MIT. Their notices are collected
  automatically by `scripts/notices.ps1`; if CI says it can't place an Android library's
  licence, add the text to `scripts/licenses/` and the table in the script.
- Describe user-visible changes under `## [Unreleased]` in [CHANGELOG.md](CHANGELOG.md). Don't
  change version numbers: the maintainer sets them with `npm run bump -- <version>` when
  releasing, and the release notes are taken from that section.
- Match the style of the surrounding code; comments explain *why*, not *what*.

By contributing you agree that your contributions are licensed under the [MIT License](LICENSE).
