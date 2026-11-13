# Xbox 360 XDK build and testing requirements

This document records what is needed to compile and test the native Xbox 360 target. It intentionally does not include SDK binaries, headers, libraries, credentials, or instructions for obtaining unauthorized copies of proprietary tools.

## 1. Which SDK this project needs

This repository's native console target is the **Xbox 360 XDK**, not the modern Xbox Game Development Kit (GDK/GDKX).

The project files make that explicit:

- `Butterscotch360.sln` is a Visual Studio 2010-format solution.
- `Butterscotch360.vcxproj` declares the `Xbox 360` platform.
- The XDK sources include headers such as `xtl.h`, `d3d9.h`, `xgraphics.h` and `xaudio2.h`.
- The project links Xbox 360 libraries such as `xapilib`, `xboxkrnl`, `xbdm`, XAudio and D3D libraries.

**Do not try to replace the Xbox 360 XDK with the public modern GDK.** Microsoft's own GDK overview says the GDK and GDKX do not support Xbox 360 development; they target newer Xbox generations and Windows.

## 2. Do we currently have the toolchain?

**No.** Neither this ChatGPT work environment nor Claude's environment currently has a licensed Xbox 360 XDK compiler, its matching Visual Studio integration, or a physical Xbox 360 development kit available.

The historic Xbox 360 XDK was a proprietary developer toolchain distributed to authorized developers. Microsoft's currently documented public GDK downloads are not a substitute. Community documentation reports that official Xbox 360 SDK access has been discontinued; that is a useful description of the current situation, but it is not a Microsoft licensing statement.

If someone on the project already has legitimate access from their own authorized development setup, that would be the best route for a real build. Do not upload or commit SDK files, private headers, libraries, credentials, or other licensed material to this repository.

Useful background:
- Microsoft: [Meet the Microsoft Game Developer Kit](https://developer.microsoft.com/en-us/games/articles/2021/07/meet-the-microsoft-game-developer-kit-gdk/) — explicitly distinguishes GDK/GDKX from Xbox 360 development.
- Microsoft: [Xbox game development resources](https://developer.microsoft.com/en-us/games/resources/) — explains that protected developer resources require authorization.
- Community historical reference: [Unofficial Xbox 360 Development Guide — development environment](https://xb360.dev/development-environment/) — documents the historical XDK environment and the current access situation.

## 3. What a real XDK build machine needs

A machine doing the native build needs all of the following, obtained and used under the applicable license:

1. A compatible Xbox 360 XDK installation, including its compiler, platform headers, libraries and MSBuild/Visual Studio integration.
2. The Visual Studio generation supported by that exact XDK installation. This solution is in the Visual Studio 2010 project format, but the installed XDK's own requirements are authoritative; do not assume a current Visual Studio version can build it.
3. The project checked out at the exact commit being tested.
4. Enough free disk space for the XDK intermediates and output.
5. For deployment/debugging, a compatible Xbox 360 development kit and the network/debug setup supported by that XDK. A retail console or emulator may be useful for later homebrew/runtime testing, but it does not replace the compiler and SDK.

The expected project configuration is `Debug|Xbox 360` or `Release|Xbox 360`. The configured intermediate/output directories are under `build-xbox360-xdk\\`; check the generated build output for the actual XEX path rather than assuming a filename.

## 4. A safe build-and-report workflow

When a licensed build machine is available:

1. Check out `refresh2/modern-upstream-migration` and record the exact commit SHA.
2. Open `Butterscotch360.sln` in the Visual Studio version supported by the installed XDK.
3. Select `Debug` or `Release` and the `Xbox 360` platform.
4. Build the solution without changing the SDK installation or copying proprietary SDK files into the repository.
5. Save the complete compiler/linker output, especially the **first** error (later errors may be consequences).
6. Report the XDK version, Visual Studio version, configuration, commit SHA, first error and relevant surrounding log lines. Do not include licensed SDK headers, binaries, secrets or private machine paths.
7. Once it builds, deploy to authorized test hardware and test in small milestones: startup, first room, sprite/text rendering, input, audio, room transitions, then application surfaces and Deltarune-specific features.

A successful Windows headless/CI build of the shared core is useful, but it is **not** proof that the XDK project compiles. The native XDK code and console libraries must be built by the actual XDK toolchain.

## 5. What we can do without an XDK

We can still make meaningful progress:

- Compare the migrated platform adapters against the modern interfaces and validate function signatures.
- Keep the shared core and non-XDK code covered by ordinary host-side builds where possible.
- Review ownership, bounds checks, state transitions and error handling in the XDK-specific code.
- Prepare minimal diagnostic builds and narrow test cases for whoever has the toolchain.
- Use emulator/hardware test results supplied by a project contributor to fix runtime issues.

What we **cannot honestly claim** without the XDK compiler is that the Xbox 360 project compiles, links, deploys, or runs. Static review is not a substitute for those checks.

## 6. If no one can access the old XDK

There are two realistic choices:

- Continue the current XDK port as a source-level migration, clearly marking it unverified until someone with a lawful XDK setup can build it.
- Investigate a separate homebrew backend based on an available open-source Xbox 360 toolchain such as libxenon. That would be a different platform port, not a drop-in replacement: the startup, graphics, audio, filesystem and build/deployment code would need a separate implementation and test plan.

Do not mix the two approaches in the same build configuration. First decide which toolchain is actually available, then make the platform backend match it.
