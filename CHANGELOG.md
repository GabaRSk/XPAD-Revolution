# Changelog

## v1.0.0 — 2026-08-16

First public package of the tested internal plugin build v4.0.5.

- Added Flydigi Direwolf 4 support while preserving Direwolf 2 through dynamic endpoint OUT discovery.
- Reduced the USB input worker interval to 2 ms with a safe 64-byte report buffer.
- Added hybrid latest-state/transition buffering for fast button presses.
- Added automatic UDP viewer discovery on port 39001 and telemetry on port 39000.
- Added real analog L2/R2 pressure to the local overlay.
- Added an offline Windows viewer installer and optional virtual Xbox 360 controller output.
- Added HEN-safe remapping with selectable profiles.
- Added USB touchpad-click remapping for DualShock 4, DualSense and DualSense Edge.
- Added configurable DS3-style analog saturation and radial deadzone correction.
- Added a unified game module for native-Bluetooth analog correction, compatibility hooks and rumble.
- Added a built-in manual game loader using SELECT + L3 + R3, removing the normal need for a PC or fixed game PID.
- Added delayed HEN-safe VSH initialization and diagnostic logging.
