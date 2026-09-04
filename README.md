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

## Multiplayer character creation

Use **Create Multiplayer Character** in the launcher. The launcher generates a
stable multiplayer-only character ID, Skyrim bootstraps a temporary player
directly into Riverwood, and the native compact character creator opens. After
the player confirms their unique appearance, the client creates
`SkyrimMP_<character-id>`. No single-player save is imported, changed, or used
as the multiplayer identity.

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

The current build provides multiplayer-only character creation, first-login
Riverwood placement, reliable appearance profiles, authenticated UDP sessions,
reconnects, authoritative player entities, interest management, remote-player
NPC-backed avatars, and safe
event-driven player combat/death status and equipment replication. It is not
yet a complete co-op Skyrim conversion: authoritative combat outcomes,
animation state, inventories, quests, dialogue, and world interactions still
need dedicated synchronization and multi-client gameplay validation.

## Current status

The current development protocol is **replication protocol 9** over wire
protocol 2. The following paths have been built and exercised against Skyrim
SE/AE with SKSE64:

- Separate launcher modes for normal single player and multiplayer.
- Multiplayer-only first-login character creation followed by a dedicated
  `SkyrimMP_*` save; no SP save import is required.
- First multiplayer login placement in Riverwood.
- Persistent server-owned character location across reconnects and restarts.
- Duplicate-character login rejection to protect MP save ownership.
- Reliable session/bootstrap traffic and interest-based entity replication.
- Unique NPC-backed remote-player avatars with reliable name, race, sex,
  weight, face, hair, head-part and body appearance profiles.
- Event-driven combat/death status transport without unsafe actor-value reads.
- Bounded equipment-set replication from `TESEquipEvent`, authoritative
  persistence across server restarts, and game-thread remote-proxy
  equip/unequip reconciliation. Live client/server add, replace, reconnect,
  and persistence paths are verified; visual two-client proxy validation is
  still required on two simultaneous game clients.
- Authoritative `QUST` definition import retained by the server runtime, with
  deterministic canonical keys and typed stages, objectives, aliases, and
  condition counts. Startup classifies every winning quest as record-driven,
  adapter-required, or unsupported instead of treating Papyrus-heavy quests
  as automatically safe.
- Deterministic `ServerQuestProgram` compilation for record-driven quests with
  ordered, deduplicated stage transitions; unsafe definitions remain excluded
  behind explicit adapter/unsupported classifications.
- A tested party quest runtime with shared instances, validated stage
  transitions, late-join projection, leave-time personal quest freezing, and
  idempotent per-character reward records. Versioned atomic disk persistence
  is loaded at server startup and flushed on graceful shutdown; client quest
  event/projection messages are the next integration layer. A bounded quest
  wire contract now defines typed gameplay evidence and authoritative client
  projections, with no client-side quest-completion operation.
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

1. Canonicalize and validate replicated equipment FormIDs against the server
   item database, then complete visual two-client loadout reconciliation tests.
2. Action and animation replication using SKSE action events plus a bounded,
   validated animation-event allowlist.
3. Server-authoritative hit validation, damage, death, resurrection, magicka,
   and stamina, without relying on unsafe early-frame actor virtual calls.
4. Inventory/container transactions with server ownership, atomic validation,
   rollback, and duplicate-item protection.
5. Compile imported quest definitions into executable server quest programs;
   add persistent parties, shared quest instances, validated progress events,
   client projections, world transitions, and idempotent reward transactions.
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
