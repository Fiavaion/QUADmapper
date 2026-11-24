# Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io), certificate by [SignPath Foundation](https://signpath.org).

QUADmapper's Windows installer (`QUADmapper-Setup-<version>.exe`) and the `QUADmapper.exe` it installs are signed. Both are built from this repository's source by the [Release installer](../.github/workflows/release.yml) workflow on GitHub-hosted runners, then signed through SignPath. Nothing built on a personal computer is signed.

The installer also contains third-party open-source libraries (FFmpeg, GLFW and others, listed in [THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md)) and the Microsoft Visual C++ runtime. Those files are shipped as their projects publish them and are not signed by this project.

## Team roles

- Committers and reviewers: [Fiavaion](https://github.com/Fiavaion)
- Approvers: [Fiavaion](https://github.com/Fiavaion)

Every signing request is approved by hand in SignPath before it is signed.

## Privacy

This program will not transfer any information to other networked systems unless specifically requested by the user or the person installing or operating it.

For example, OSC control listens on the network only after the user turns it on, and Help > Report a Bug opens the issue page in a web browser only when the user chooses it.
