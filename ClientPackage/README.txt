SkyrimMultiplayer test client
=============================

This is a DEVELOPMENT TEST BUILD. It is not a finished multiplayer release.

Package channel: re-0.1-runtime-probe / controlled remote-player proxy test.

Requirements
------------
- Windows x64
- Skyrim Special Edition
- SKSE64 compatible with the installed Skyrim runtime
- The same required game/master content as the test server
- Access to the SkyrimMP server over UDP port 10578 for multiplayer

Install
-------
1. Extract this package somewhere outside the Skyrim folder.
2. Run Install-Client.ps1.
3. Enter the Skyrim Special Edition installation folder when asked.
4. After installation, use "SkyrimMP Launcher.cmd" to start Skyrim.

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

Do not redistribute Bethesda game assets, SKSE, Creation content, or third-party mods with this package. This package contains only the SkyrimMultiplayer test DLL and our helper/launcher scripts.
