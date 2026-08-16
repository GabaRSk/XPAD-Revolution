# Changelog

## XPAD Revolution v1.0.0 — 2026-08-16

- Rebranded the complete project, PS3 notifications and Windows viewer as XPAD Revolution.
- Added PS3HEN-safe delayed VSH initialization.
- Added dynamic USB interrupt-OUT endpoint discovery for compatible controllers.
- Reduced the USB input worker interval to 2 ms with a safe 64-byte report buffer.
- Added hybrid latest-state/transition buffering for fast button presses.
- Added HEN-safe remapping with selectable profiles and USB DS4/DualSense touchpad-click sources.
- Added configurable DS3-style analog saturation and radial deadzone correction.
- Added a unified game module for native-Bluetooth analog correction, compatibility hooks and rumble.
- Added the built-in SELECT + L3 + R3 game-module loader, avoiding fixed game PIDs and normal PC dependency.
- Added automatic UDP viewer discovery, real L2/R2 pressure and an optional virtual Xbox 360 controller.
- Added an offline Windows installer that configures the local firewall rule and migrates the previous viewer registration.
