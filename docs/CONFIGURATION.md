# Configuration

## Remapping

Edit `/dev_hdd0/plugins/ps3xpad/xpad_remap.txt`:

```text
PHYSICAL_BUTTON = BUTTON_SENT_TO_PS3
```

Example:

```text
R1 = R2
R2 = R1
```

`START_ENABLED = 1` enables the selected profile at startup. With it disabled, toggle the profile with **START + SELECT + DPAD RIGHT**.

For a USB-connected DS4/DualSense:

```text
TOUCHPAD = SELECT
```

or:

```text
TOUCHPAD = START
```

Touchpad is a physical source only. Native PS3 Bluetooth removes that bit before XPAD Revolution can read it, so touchpad remapping requires USB.

The viewer intentionally displays physical input before remapping; the PS3 receives the remapped result.

## Analog curve

Edit `/dev_hdd0/plugins/ps3xpad/xpad_analog.txt`:

```text
ANALOG_SATURATION = 80
ANALOG_DEADZONE = 4
ANALOG_GAME_MODE = AUTO
ANALOG_PORT_MASK = 1
```

Suggested saturation values:

- `80`: general DS3-style baseline.
- `75`: reaches corners earlier.
- `85`: smoother response.
- `100`: no saturation correction.

Suggested deadzone values:

- `4`: conservative default.
- `0`: suitable for a drift-free Hall-effect controller.

`ANALOG_PORT_MASK` is a bit mask: `1` is port 1, `2` is port 2 and `3` is both. Keep `ANALOG_GAME_MODE = AUTO` unless a specific firmware or adapter misclassifies the controller; FORCE can also affect a real DS3 or VSH-created LDD pad.

The VSH module applies the curve to supported XInput and USB DS4/DualSense input. The game module applies it to selected native-Bluetooth pads after `cellPadGetData`.

## Logs

VSH log: `/dev_hdd0/plugins/ps3xpad/xpad.log`

Game-module log: `/dev_hdd0/plugins/ps3xpad/xpad_game.log`
