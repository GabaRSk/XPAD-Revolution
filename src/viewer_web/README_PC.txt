XPAD Revolution 1.0.0 - Local Overlay + Virtual Gamepad
=======================================================

The Windows app automatically announces the PC address to the PS3 on UDP 39001
and receives controller telemetry on UDP 39000. No fixed PC or console IP is
required.

LOCAL XPAD REVOLUTION VIEWER

Open http://127.0.0.1:8765/?pad=0 after starting the app. L2 and R2 use their
real 0-255 pressure. Add &debug=1 to show the numeric trigger values.

STANDARD GAMEPAD VIEWER

The app can mirror PS3 Pad 1 as a local Xbox 360 controller. This lets
https://gamepadviewer.com/ and other browser-based viewers read the PS3 inputs
through the normal Windows Gamepad API.

1. Run Instalar_XPAD_Revolution.exe as administrator.
2. Leave "Ativar controle virtual Xbox 360" selected.
3. Confirm the official ViGEmBus driver installer when it appears.
4. Start the PS3 plugin and wait for "PS3 detectado" in the Windows app.
5. Click "Gamepad Viewer padrao" and press any controller button once so the
   browser enables the gamepad.

The virtual controller mirrors Pad 1. Button transitions, sticks, both analog
triggers and the PS/Guide button are translated. If PS3 packets stop for one
second, the virtual controller is immediately returned to a neutral state so
no button or stick remains held.

The optional virtual-controller component uses the final official ViGEmBus
1.22.0 release. That project is archived. Version 1.22.0 removed its old
updater. See THIRD_PARTY_NOTICES.txt for source URLs, licenses and the pinned
driver hash.

UNINSTALL

Removing XPAD Revolution also removes its UDP 39000 Firewall rule. ViGEmBus is
left installed because it is a shared driver that other controller tools may
also use.
