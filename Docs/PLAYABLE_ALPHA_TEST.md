# SkyrimMP playable-alpha two-PC test

Build identity: **0.1.0-alpha.2**, wire protocol **2**, replication protocol **8**.

Both PCs run Skyrim SE/AE, matching SKSE, and the SkyrimMP client. Only one
`SkyrimMPServer.exe` is started. Both launchers must point to that server's IPv4
address and UDP port 10578. Allow inbound UDP 10578 through Windows Firewall on
the server PC. Never use the same character save on both PCs.

## Before testing

- Install artifacts from the same successful GitHub Actions run on both PCs.
- Confirm the launcher title, each SKSE log, and server startup report the build
  and protocol values above.
- On each PC, create a different character in single player, leave the Helgen
  cave, and make a normal save. Keep the original saves unchanged.
- Create a timestamped folder for the server log and each client log.

## Test run

Record PASS or FAIL and relevant log timestamps for every row.

| # | Action | Expected result | Result / evidence |
|---:|---|---|---|
| 1 | Start the dedicated server. | Startup self-tests pass; UDP 10578 listens; version/protocol match. | |
| 2 | Start PC 1 with **Join Multiplayer Server**. | Launcher writes the direct server target and Skyrim opens through SKSE. | |
| 3 | Load PC 1's post-Helgen save, or its existing `SkyrimMP_*` save. | First import moves to Riverwood and creates a separate MP save; later logins restore it without crashing. | |
| 4 | Start PC 2 with **Join Multiplayer Server**. | It connects to the same dedicated server. | |
| 5 | Load PC 2's distinct character/save. | A distinct `SkyrimMP_*` branch is created or restored. | |
| 6 | Inspect the server log. | Two sessions, two character IDs, and two player entity IDs are present. | |
| 7 | Meet in Riverwood. | Each client sees only the other character; no self or duplicate proxy exists. | |
| 8 | Walk, run, rotate, sneak, and jump. | Remote movement and state remain recognizable and responsive. | |
| 9 | Draw and sheathe weapons. | The remote proxy matches both transitions. | |
| 10 | Equip and unequip several weapons and armor pieces. | Visible equipment converges; authoritative revisions advance. | |
| 11 | Attack, block, and cast. | The remote proxy shows each basic action without crashing. | |
| 12 | Take damage, die, and recover. | Health/combat/death visibility is safe and does not resurrect from stale state. | |
| 13 | Move through exterior cells and enter/leave an interior. | Proxy despawns/spawns in the correct worldspace/cell without duplication. | |
| 14 | Close PC 2 Skyrim. | PC 2 times out/despawns; PC 1 and the server remain stable. | |
| 15 | Reconnect PC 2 with the same MP save. | Its persisted character/entity state is restored. | |
| 16 | Inspect both clients. | No duplicate proxy or late stale-packet resurrection appears. | |
| 17 | Cleanly stop and restart the server. | Authoritative character/equipment state is saved and reloaded. | |
| 18 | Reconnect both clients. | Both remain distinct and persisted MP state converges again. | |

## Evidence to retain

- Server console output from startup through final shutdown.
- `%USERPROFILE%\\Documents\\My Games\\Skyrim Special Edition\\SKSE\\SkyrimMultiplayer.log` from each PC.
- Launcher version shown on each PC and the downloaded artifact names/run number.
- Character ID, session ID, network entity ID, replication revision, endpoint,
  rejection reason, spawn/despawn, reconnect, and timeout lines surrounding any failure.

A real two-PC PASS requires two physical Skyrim clients connected concurrently.
Compilation, self-tests, or two synthetic UDP sessions do not substitute for it.
