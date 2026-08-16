# Installation

## 1. Install the PS3 plugin

Back up the currently working plugin first. From the release ZIP, copy these files to `/dev_hdd0/plugins/ps3xpad/`:

```text
xpad_vsh_autodiscovery.sprx
xpad_game.sprx
xpad_devices.txt
xpad_remap.txt
xpad_analog.txt
```

Add only this line to `/dev_hdd0/boot_plugins.txt`:

```text
/dev_hdd0/plugins/ps3xpad/xpad_vsh_autodiscovery.sprx
```

`xpad_vsh_autodiscovery.sprx` is the delayed HEN-safe XMB module. `xpad_game.sprx` has a different game-process signature and must stay outside `boot_plugins.txt`.

Reboot the console, enable HEN and allow roughly ten seconds for initialization. The on-screen message should report `XPAD v4.0.5 Loaded!`.

## 2. Load the game companion without a PC

When a game has reached its menu, hold:

```text
SELECT + L3 + R3
```

for approximately 0.8 seconds. On DS4, SELECT is SHARE; on DualSense it is CREATE. The VSH module finds the active EBOOT process and loads `xpad_game.sprx`. A successful load shows:

```text
XPAD: Bluetooth game plugin active
```

The loader re-arms after the game exits. A webMAN/PS3MAPI fallback remains under `extras/webMAN/`, but it is normally unnecessary.

## 3. Install the Windows viewer

1. Run `Instalar_PS3xPAD_Viewer.exe` as administrator.
2. Keep the virtual Xbox 360 controller option selected if you want standard browser viewers.
3. Confirm the official ViGEmBus installer when Windows displays it.
4. Start the installed viewer and wait for it to report the detected PS3.

No fixed IP address is required. The app announces itself on UDP 39001 and receives telemetry on UDP 39000. The installer creates the required Windows Firewall rule.

Local overlay:

```text
http://127.0.0.1:8765/?pad=0
```

Add `&debug=1` for numeric trigger pressure. For the standard viewer, use the app's **Gamepad Viewer** button and press any controller button once so the browser enables the Gamepad API.

## Recovery

If the VSH module causes a problem, restore the previous VSH SPRX using your recovery/FTP method. If only one game has a problem, restart the game and do not activate the game-module hotkey; `xpad_game.sprx` is not loaded at boot.
