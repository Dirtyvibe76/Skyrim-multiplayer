SkyrimMultiplayer two-player test client
=======================================

This is a DEVELOPMENT TEST BUILD. It is not a finished multiplayer release.

Package channel: re-0.1-runtime-probe / controlled one-remote-player proxy test.

Requirements
------------
- Windows x64
- Skyrim Special Edition
- SKSE64 compatible with the installed Skyrim runtime
- The same required game/master content as the test server
- Access to the SkyrimMP server over UDP port 10578

Install
-------
1. Extract this package somewhere outside the Skyrim folder.
2. Right-click Install-Client.ps1 and run it with PowerShell, or run:

   powershell -ExecutionPolicy Bypass -File .\Install-Client.ps1

3. Enter the Skyrim Special Edition installation folder when asked.

Connect
-------
The current test DLL still targets 127.0.0.1:10578. The included relay forwards that local traffic to the actual remote server.

1. Run:

   powershell -ExecutionPolicy Bypass -File .\Connect-To-SkyrimMP.ps1

2. Enter the SERVER COMPUTER'S IPv4 address.
3. Leave that PowerShell window open.
4. Launch Skyrim through SKSE64.
5. Load a save and move into the same CELL / nearby world area as the other player.

If both players are in different houses/networks
------------------------------------------------
The server owner must forward UDP port 10578 from the router to the PC running SkyrimMPServer.exe and allow SkyrimMPServer.exe / UDP 10578 through Windows Firewall. The connecting player uses the server owner's public IPv4 address in Connect-To-SkyrimMP.ps1.

If both players are on the same LAN
-----------------------------------
Use the server PC's LAN IPv4 address (usually 192.168.x.x or 10.x.x.x). Router port forwarding is not required.

Current controlled test limitation
----------------------------------
The native client proxy layer is intentionally limited to ONE remote player proxy. That is exactly what is needed for this two-player validation.

Do not redistribute Bethesda game assets, SKSE, Creation content, or third-party mods with this package. This package contains only the SkyrimMultiplayer test DLL and our helper scripts.
