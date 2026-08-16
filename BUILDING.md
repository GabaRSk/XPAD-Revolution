# Building

## PS3 modules

The verified build environment used:

- Sony Cell SDK 4.75
- `ppu-lv2-gcc` 4.1.1
- `scetool` 0.2.14

Source layout:

- `src/ps3/main.c` + `src/ps3/libc.c` build the VSH module.
- `src/game/xpad_game.c` + `src/ps3/libc.c` build the game-process companion.
- `src/ps3/xpad_analog.h` contains the shared analog curve.

The repository deliberately excludes all Cell SDK files, proprietary headers/libraries, PPU tools, `scetool` binaries/data and signing material. Set up those dependencies separately and review the included Makefiles for local paths.

The tested HEN outputs use key revision `0A`, with separate authentication IDs for VSH and game processes. Never rename one signed module over the other.

## Windows viewer

The viewer sources are under `src/viewer_windows/`. `build_installer.ps1` compiles the viewer and wrapper installer with the .NET Framework C# compiler available on Windows.

Virtual-controller support expects these official dependencies in the paths referenced by the build script:

- Nefarius.ViGEm.Client 1.21.256
- ViGEmBus 1.22.0 final installer

They are embedded in the published offline installer but are not duplicated in this source tree. Their pinned hashes and licenses are in `THIRD_PARTY_NOTICES.txt`.
