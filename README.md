# PS3xPAD HEN — Direwolf 4 Low-Latency Edition

Community-maintained PS3xPAD build focused on PS3HEN, Flydigi Direwolf 2/4 support, low-latency input, remapping, analog correction and controller telemetry for Windows.

> GitHub release **v1.0.0** packages the tested internal plugin build **v4.0.5**. The different numbers are intentional: v1.0.0 is the first public release of this community edition.

## Highlights

- PS3HEN-safe VSH startup with a 10-second boot delay.
- 2 ms / 500 Hz USB input worker and a safe 64-byte report buffer.
- Dynamic endpoint OUT discovery for Direwolf 2 (`0x02`) and Direwolf 4 (`0x05`).
- Hybrid low-latency buffering that keeps the latest state without dropping quick button transitions.
- HEN-safe remapping, including DS4/DualSense touchpad click as a source over USB.
- Configurable DS3-style radial deadzone and analog saturation curve.
- Unified game module for native Bluetooth DS4/DualSense analog correction, compatibility hooks and rumble.
- Built-in game-module loader: hold **SELECT + L3 + R3 for 0.8 seconds** after the game reaches its menu.
- Automatic UDP discovery: no fixed PC or PS3 IP address.
- Local browser overlay with real L2/R2 pressure.
- Optional Windows virtual Xbox 360 controller for standard browser gamepad viewers.

## Download

Download `PS3xPAD-HEN-Direwolf4-v1.0.0.zip` from the [latest release](../../releases/latest). The ZIP contains the ready-to-copy PS3 files and the offline Windows viewer installer.

## Quick installation

1. Back up your current `/dev_hdd0/plugins/ps3xpad/` directory.
2. Copy the five files from `plugin/ps3xpad/` in the release ZIP to `/dev_hdd0/plugins/ps3xpad/`.
3. Add only this line to `/dev_hdd0/boot_plugins.txt`:

   ```text
   /dev_hdd0/plugins/ps3xpad/xpad_vsh_autodiscovery.sprx
   ```

4. Do **not** put `xpad_game.sprx` in `boot_plugins.txt` and do not rename one SPRX over the other.
5. Reboot, enable HEN and wait for `XPAD v4.0.5 Loaded!`.
6. Inside a game, hold **SELECT + L3 + R3 for 0.8 seconds** to load the game module when needed.

See [Installation](docs/INSTALLATION.md) and [Configuration](docs/CONFIGURATION.md) for the complete guide.

## Important Bluetooth limitation

Native PS3 Bluetooth removes some DS4/DualSense-specific information before the plugin receives the pad data. The analog correction works, but native-Bluetooth touchpad click cannot be recovered and the VSH telemetry viewer cannot see that native pad. Use USB for touchpad remapping and complete viewer data.

## Source and build requirements

The repository contains the modified PS3 and Windows viewer sources. It intentionally does **not** contain Sony Cell SDK files, the PPU toolchain, `scetool`, signing data or old intermediate objects. See [BUILDING.md](BUILDING.md).

## Credits and licensing

This is a derivative community build of **PS3xPAD by OsirisX**. See [CREDITS.md](CREDITS.md) and [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).

The upstream PS3xPAD archives used for this work did not include a clear repository-wide license. No new blanket license is asserted here over upstream code. Individual third-party files retain their own notices and licenses.

Use this software at your own risk. Keep a working backup and a recovery/FTP path before replacing a boot plugin.

