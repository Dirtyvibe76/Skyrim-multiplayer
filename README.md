# Skyrim Multiplayer

Server-authoritative Skyrim multiplayer project.

## Architecture

- ClientPlugin: Skyrim/SKSE integration layer
- Server: Dedicated authoritative multiplayer server
- Shared: Shared state/data structures
- Protocol: Network messages and serialization
- ReverseEngineering: Skyrim runtime probes, hooks, offsets, and notes
- Tests: Protocol/state tests
- Docs: Architecture and research notes

## Core rule

The server owns authoritative multiplayer state.
Clients provide input, rendering, local prediction, and engine integration.

## Dedicated server

Build the server from the repository root:

```powershell
xmake build SkyrimMPServer
```

Copy `Server/server.ini.example` to `server.ini`, set `Game.DataPath` to an owned
Skyrim Special Edition `Data` directory, and configure the hosted mods plus
`plugins.txt` and `loadorder.txt`. Then run:

```powershell
build\windows\x64\releasedbg\SkyrimMPServer.exe server.ini
```

The selected configuration controls bootstrap and the live server. `Port` must
be between 1 and 65535, `MaxPlayers` must be nonzero, and `TickHz` must be from
1 through 120. The server validates the plugin stack, builds the authoritative
world, runs its protocol and socket self-tests, and only then begins listening.
Press Ctrl+C for a graceful shutdown.

## Single-player character import

Use **Play Single Player** to create a character, finish Helgen, exit the cave,
and make a normal save. Then use **Join Multiplayer Server** and load that save.
The server recognizes the loaded Skyrim character, places it at Riverwood on
its first multiplayer login, and the client immediately creates a separate
`SkyrimMP_<character-id>` save. Later multiplayer sessions use that branch so
multiplayer progress diverges without overwriting the original post-Helgen save.

First-login completion is recorded in `server-data/first-logins.txt`, so a
dedicated-server restart does not send established characters to Riverwood
again. Skyrim SE or AE and a matching SKSE64 installation remain required on
every client.

Each multiplayer character's authoritative location is stored separately in
`server-data/players/<character-id>.state`. The server restores that state on
reconnect, checkpoints dirty characters at five-second intervals, and flushes
pending changes during graceful shutdown. A character identity may only be
online once at a time, preventing two clients from racing the same MP save.

## Current playable scope

The current build provides the SP-to-MP save branch, first-login Riverwood
placement, authenticated UDP sessions, reconnects, authoritative player
entities, interest management, remote-player transform proxies, and safe
event-driven player combat/death status replication. It is not yet a complete
co-op Skyrim conversion: authoritative combat outcomes, equipment and
animation state, inventories, quests, dialogue, and world interactions still
need dedicated synchronization and multi-client gameplay validation.

## Current status

The current development protocol is **replication protocol 6** over wire
protocol 2. The following paths have been built and exercised against Skyrim
SE/AE with SKSE64:

- Separate launcher modes for normal single player and multiplayer.
- Post-Helgen character import followed by a dedicated `SkyrimMP_*` save.
- First multiplayer login placement in Riverwood.
- Persistent server-owned character location across reconnects and restarts.
- Duplicate-character login rejection to protect MP save ownership.
- Reliable session/bootstrap traffic and interest-based entity replication.
- Native remote-player proxies with transform updates and despawning.
- Event-driven combat/death status transport without unsafe actor-value reads.
- Automated protocol, UDP loopback, session, duplicate-login, entity, and
  replication startup self-tests.

The save-load crash found during AE testing is fixed. Do not move direct
`GetActorValue`/combat virtual calls back into the earliest
`PlayerCharacter::Update` hook; gameplay state should continue to be captured
from engine events or another verified-safe main-thread phase.

## Build requirements

### Client

- Windows x64.
- Visual Studio 2022 C++ build tools and a Windows SDK.
- [xmake](https://xmake.io/) with C++23 support.
- .NET 8 SDK for the standalone launcher.
- The checked-out `External/CommonLibSSE-NG` git submodule.
- For runtime testing: an owned Skyrim Special Edition or Anniversary Edition
  installation and the matching SKSE64 release.

Build the plugin and launcher from the repository root:

```powershell
git submodule update --init --recursive
xmake f -m releasedbg -a x64 -y
xmake build SkyrimMultiplayer
dotnet publish ClientLauncher\SkyrimMPLauncher.csproj -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -o launcher-publish
```

### Dedicated server

- Windows x64, Visual Studio 2022 C++ build tools, Windows SDK, and xmake.
- An owned Skyrim SE/AE `Data` directory containing the base master files.
- A server `server.ini`, `plugins.txt`, `loadorder.txt`, and hosted mod files
  matching the clients. Local examples and runtime data are intentionally not
  committed.

The GitHub Actions workflow builds downloadable client and server artifacts on
pushes to `re-0.1-runtime-probe` and can also be started manually.

## Future build needs

Work required before calling this a complete co-op game:

1. Equipment/loadout replication using `TESEquipEvent`, canonical FormIDs, and
   safe main-thread equip/unequip application on remote proxies.
2. Action and animation replication using SKSE action events plus a bounded,
   validated animation-event allowlist.
3. Server-authoritative hit validation, damage, death, resurrection, magicka,
   and stamina, without relying on unsafe early-frame actor virtual calls.
4. Inventory/container transactions with server ownership, atomic validation,
   rollback, and duplicate-item protection.
5. Shared quest policy and synchronization for stages, objectives, aliases,
   scenes, dialogue, and quest-specific world changes.
6. Authoritative activation state for doors, locks, traps, furniture, dropped
   objects, crafting, harvesting, and other persistent world interactions.
7. NPC AI ownership and migration so exactly one authority drives each actor.
8. Security hardening: challenge-based identity, replay protection, rate
   limits, malformed-packet fuzzing, and configurable administration tools.
9. Performance work: indexed transfer anchors, persistence batching, bandwidth
   budgets, prioritization, interpolation, and long-running soak tests.
10. Two-machine end-to-end test coverage for join/leave, combat, death,
    interiors, fast travel, save/reload, server restart, latency, and packet
    loss before a public release.
