# XPAD Revolution

[English](README.md) | [Português do Brasil](README.pt-BR.md)

XPAD Revolution is a community-maintained evolution of PS3xPAD for PS3HEN. Version 1.0.0 combines safe boot behavior, broad USB controller support, low-latency input, remapping, analog correction and controller telemetry for Windows.

## Highlights

- PS3HEN-safe VSH startup with a 10-second boot delay.
- 2 ms / 500 Hz USB input worker with a safe 64-byte report buffer.
- Dynamic interrupt-OUT endpoint discovery for compatible controllers.
- Hybrid low-latency buffering that keeps the latest state without dropping quick button transitions.
- HEN-safe remapping, including DS4/DualSense touchpad click as a source over USB.
- Configurable DS3-style radial deadzone and analog saturation curve.
- Unified game module for native-Bluetooth DS4/DualSense analog correction, compatibility hooks and rumble.
- Built-in game-module loader: hold **SELECT + L3 + R3 for 0.8 seconds** after the game reaches its menu.
- Automatic UDP discovery with no fixed PC or PS3 IP address.
- Local browser overlay with real L2/R2 pressure.
- Optional Windows virtual Xbox 360 controller for standard browser gamepad viewers.

Supported examples include Xbox 360-compatible XInput devices, Flydigi Direwolf 2/4, Logitech pads, 8BitDo/GameSir-compatible dongles, DualShock 4, DualSense and DualSense Edge. Extra USB VID/PID entries can be added in `xpad_devices.txt`.

## Download

Download `XPAD-Revolution-v1.0.0.zip` from the [latest release](../../releases/latest). It contains the ready-to-copy PS3 files and the offline Windows viewer installer.

## Quick installation

1. Back up `/dev_hdd0/plugins/ps3xpad/`.
2. Copy the five files from `plugin/ps3xpad/` in the release ZIP to `/dev_hdd0/plugins/ps3xpad/`.
3. Add only this line to `/dev_hdd0/boot_plugins.txt`:

   ```text
   /dev_hdd0/plugins/ps3xpad/xpad_vsh_autodiscovery.sprx
   ```

4. Do **not** add `xpad_game.sprx` to `boot_plugins.txt` and do not rename one SPRX over the other.
5. Reboot, enable HEN and wait for `XPAD Rev v1.0.0 Loaded!`.
6. Inside a game, hold **SELECT + L3 + R3 for 0.8 seconds** to load the game module when needed.

See [Installation](docs/INSTALLATION.md) and [Configuration](docs/CONFIGURATION.md) for the complete guide.

## Native Bluetooth limitation

Native PS3 Bluetooth removes some DS4/DualSense-specific information before the plugin receives pad data. Analog correction and game compatibility hooks work, but native-Bluetooth touchpad click cannot be recovered and VSH telemetry cannot see that native pad. Use USB for touchpad remapping and complete viewer data.

## Source and build requirements

The repository contains the modified PS3 and Windows viewer sources. It intentionally excludes Sony Cell SDK files, the PPU toolchain, `scetool`, signing data and intermediate objects. See [BUILDING.md](BUILDING.md).

## Credits and licensing

XPAD Revolution is derived from **PS3xPAD by OsirisX**. See [CREDITS.md](CREDITS.md) and [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).

The upstream archives used for this work did not include a clear repository-wide license. No new blanket license is asserted over upstream code. Individual third-party files retain their own notices and licenses.

Use this software at your own risk. Keep a working backup and a recovery/FTP path before replacing a boot plugin.
