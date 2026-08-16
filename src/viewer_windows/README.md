# XPAD Revolution Windows viewer source

`ViewerApp.cs` receives XPAD Revolution UDP telemetry, serves the local overlay and optionally mirrors Pad 1 to a virtual Xbox 360 controller. `Installer.cs` builds the elevated offline wrapper that installs the viewer, adds the UDP firewall rule and launches the official ViGEmBus installer when selected.

Build example from PowerShell:

```powershell
.\build_installer.ps1 -ViewerRoot ..\viewer_web
```

Before building, provide the two official dependencies expected by the script:

```text
dependencies/client_1.21.256/lib/netstandard2.0/Nefarius.ViGEm.Client.dll
dependencies/ViGEmBus_1.22.0_x64_x86_arm64.exe
```

The source tree does not vendor those binaries. Verify them against the hashes in `THIRD_PARTY_NOTICES.txt`. The published release installer embeds the verified copies for offline installation.

The included tests exercise packet parsing, discovery, trigger pressure, virtual-controller mapping, stale-input neutralization and the missing-driver fallback.
