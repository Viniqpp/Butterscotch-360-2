# Butterscotch 360

An unofficial community project bringing the modern [Butterscotch](https://github.com/ButterscotchRunner/Butterscotch) GameMaker runner to the **Xbox 360**.

The project builds on the original [Butterscotch 360 port](https://github.com/ceilingtilefan/Butterscotch-360), updating its runtime integration and Xbox-specific platform code to work with the modern runner.

> **Status: Experimental.** The runner has been built with the Xbox 360 XDK and launched on an RGH Xbox 360. Undertale has reached gameplay in an initial test, while Deltarune Chapter 1 currently reaches the game loop but displays a black screen and produces no audible output. Compatibility is still being developed and tested.

## What is this?

Butterscotch is a reimplementation of the GameMaker: Studio runner. It executes the game's original data and bytecode; it is not a remake or a collection of game-specific ports.

The goal of Butterscotch 360 is to bring that runner to Xbox 360 hardware.

Current compatibility targets:
- **Undertale** — the primary compatibility target.
- **Deltarune** — investigate support for its chapters where the runtime and Xbox 360 hardware make it technically possible.
- **Other compatible GameMaker games** — possible in principle, but compatibility depends on the game's runtime version, features and data format.

These are project goals, not promises of compatibility. A game starting successfully does not mean that it is fully playable.

## Current status

The modern runtime has been integrated with Xbox 360 platform adapters:

| Area | Current state |
| --- | --- |
| Modern runner and VM | Integrated |
| Xbox 360 D3D9 renderer | Functional baseline; several advanced features remain unsupported |
| XAudio2 audio backend | Modern audio interface connected; feature and playback testing ongoing |
| Xbox 360 filesystem | Modern filesystem interface implemented |
| Controller input and main loop | Integrated |
| XDK build | Built locally with XDK 21256.3 during development |
| Console compatibility | Early testing; pre-release |

### Known limitations

- Deltarune Chapter 5's Flowery battle: the background tower and the song lyrics do not appear yet (the game routes them through a shader extension the runner does not implement).
- Texture pages load in the background, so sprites can pop in a moment late the first time they are drawn. A lossless texture cache exists but does not activate under Xenia yet.
- Depth testing (`gpu_set_ztestenable`), perspective-correct texturing and `surface_get_pixels` are not implemented. Sprites created from surfaces cannot be deleted.
- When texture memory runs short, pages are DXT5-compressed, which can shift colours slightly.

### Game testing

Tested mainly in Xenia Canary, with earlier runs on an RGH console:

- **Undertale:** boots and plays through the intro.
- **Deltarune launcher:** chapter select works, including switching chapters (Xenia asks to restart on a chapter switch).
- **Deltarune Chapter 3:** reaches file select. **Chapter 5:** plays into the Flowery battle, apart from the issues above.
- Chapters 1, 2 and 4 have not been retested with this build.

These are spot checks, not full playthroughs.

## Building

The Xbox 360 target requires a compatible Windows development environment:
- Visual Studio 2010 with Service Pack 1
- Xbox 360 XDK 21256.3
- The XDK's Xbox 360 compiler, libraries and MSBuild integration

The XDK is proprietary software and is **not included in this repository**. Install it separately and make sure the `XEDK` environment variable points to the installation.

Check the environment:
```powershell
powershell -ExecutionPolicy Bypass -File tools\xdk\build-xdk.ps1 -CheckOnly
```

Build the Release XEX:
```powershell
powershell -ExecutionPolicy Bypass -File tools\xdk\build-xdk.ps1 -Configuration Release -Package
```

The packaged output is placed under `dist\butterscotch\`. To stage a game's data alongside the executable, pass its `data.win` path:
```powershell
powershell -ExecutionPolicy Bypass -File tools\xdk\build-xdk.ps1 -Configuration Release -Package -DataWin "C:\path\to\game\data.win"
```

For the complete setup, troubleshooting, smoke-test and deployment instructions, see [Building with XDK 21256.3](docs/XDK_21256_BUILD_AND_DEPLOY.md).

## Running on an RGH Xbox 360

The project is currently tested on an RGH console using Aurora. The packaged runner and the game's required data files must be placed together in the same game directory. Transfer the files over FTP or another method supported by your setup, then launch the XEX through Aurora.

Keep the game's own data files with the executable. Depending on the game, this can include `data.win`, audio-group files, external audio assets and configuration files. Missing external assets may cause individual game features to fail.

The runner writes diagnostics to:
```
game:\butterscotch.log
```

When reporting a problem, include the game/version, console or emulator, build/commit used, what happened, and the relevant log.

## Testing and contributing

Testing reports are especially useful at this stage. If you try a build, please report:
1. The game and exact version tested.
2. Whether it boots, reaches a menu, enters gameplay and can transition between rooms.
3. Whether graphics, audio and controller input work.
4. Any crashes, freezes, missing assets or visual/audio problems.
5. The build or commit identifier and the complete runner log, if available.

Please distinguish between a game that launches, one that reaches gameplay and one that has been tested thoroughly. Emulator results are useful, but should be identified separately from results on real Xbox 360 hardware.

Bug reports and focused contributions are welcome. The project is experimental, so regressions and incomplete features are expected.

## Development approach

This is a human-directed, AI-assisted engineering project. ChatGPT and Claude have assisted with research, code review, implementation, debugging and documentation. Their output is reviewed and tested rather than treated as automatically correct. Build success, runtime behavior and compatibility are tracked as separate things.

## A personal note from Vinny

**TO BE WRITTEN**

## Credits and acknowledgements

- [Butterscotch](https://github.com/ButterscotchRunner/Butterscotch) — the upstream open-source GameMaker runner.
- [Butterscotch 360](https://github.com/ceilingtilefan/Butterscotch-360) — the original community Xbox 360 port and the starting point for this work.

This is an unofficial community project and is not affiliated with or endorsed by Toby Fox, the creators or publishers of Undertale or Deltarune, YoYo Games/GameMaker, Microsoft, or the upstream Butterscotch maintainers.

---

## Upstream Butterscotch overview

The following overview is retained from the upstream project README for background on the runner and its goals. For the latest upstream information, documentation and supported platforms, visit the [upstream repository](https://github.com/ButterscotchRunner/Butterscotch).