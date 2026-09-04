SkyrimMultiplayer test client
=============================

This is PLAYABLE ALPHA 0.1.0-alpha.1. It is not a finished multiplayer release.

Package channel: re-0.1-runtime-probe
Wire protocol: 2
Replication protocol: 8

The launcher, client log, and server startup log must all show this same version
and protocol pair. Do not mix artifacts from different workflow runs.

Requirements
------------
- Windows x64
- Skyrim Special Edition or Anniversary Edition
- SKSE64 compatible with the installed Skyrim runtime
- The same required game/master content as the test server
- Access to the SkyrimMP server over UDP port 10578 for multiplayer

Recommended install
-------------------
1. Download the SkyrimMP-Setup artifact from the matching successful GitHub Actions run.
2. Extract the artifact ZIP and run SkyrimMP-Setup.exe.
3. The installer places SkyrimMP under your local Windows profile and creates Start Menu shortcuts.
4. Optionally select the desktop-shortcut task during setup.
5. Launch SkyrimMP Launcher.
6. On first run, choose your own Skyrim SE/AE installation folder.
7. Enter the server IPv4 address or hostname and UDP port.

Portable package
----------------
The SkyrimMultiplayer-Client artifact remains available for development/testing. Extract it outside the Skyrim folder and run SkyrimMPLauncher.exe directly.

Launcher modes
--------------
Play Single Player
- Disables Data\SKSE\Plugins\SkyrimMultiplayer.dll for this and future single-player launches.
- Launches Skyrim SE/AE through SKSE without connecting to a SkyrimMP server.

Join Multiplayer Server
- Re-enables SkyrimMultiplayer.dll if Single Player mode disabled it.
- Load a single-player save made after leaving the Helgen cave. On the first
  multiplayer login, the server moves that character to Riverwood and creates
  a separate SkyrimMP_<character-id> save. Continue multiplayer from that save;
  the original single-player save is preserved.
- Starts the included local UDP relay.
- Launches Skyrim through skse64_loader.exe.
- Uses the server IPv4 / hostname entered in the launcher.

Both modes require a locally owned Skyrim Special Edition or Anniversary Edition
installation and a matching SKSE64 build. The dedicated server remains headless
and authoritative for multiplayer sessions; Skyrim supplies the client game engine
and locally owned game assets.

LAN multiplayer
---------------
Use the server PC's LAN IPv4 address (normally 192.168.x.x or 10.x.x.x). Router port forwarding is not required. Windows Firewall must allow the dedicated server on UDP 10578.

Internet multiplayer
--------------------
The server owner must forward UDP port 10578 from the router to the PC running SkyrimMPServer.exe and allow SkyrimMPServer.exe / UDP 10578 through Windows Firewall. Remote players use the server owner's public IPv4 address in the launcher.

Current multiplayer capacity
----------------------------
The default server limit is 64 players. Each client can materialize up to 63
remote-player proxies and excludes its own authoritative player entity.

Uninstall
---------
Use Windows Settings > Apps > Installed apps > SkyrimMP > Uninstall. The installer does not delete Skyrim saves.

Do not redistribute Bethesda game assets, SKSE, Creation content, or third-party mods with this package. This package contains only SkyrimMP-owned binaries, launcher files, documentation, and hashes.
