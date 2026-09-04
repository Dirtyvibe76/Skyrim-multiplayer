# Closed alpha rules

The closed alpha is one authoritative dedicated server with two or more Skyrim
clients.  Normal Skyrim single player remains separate and untouched.

## Character entry

- A multiplayer import begins from a distinct, post-Helgen, level-one source
  character.
- The source save remains unchanged.  Multiplayer creates and thereafter uses
  a separate `SkyrimMP_<character-id>` branch.
- The future import inventory gate permits only an explicit Helgen loot
  whitelist; all other imported equipment, consumables, materials, and gold
  are removed from the MP branch exactly once under server authority.

## World, death, and travel

- The server owns persisted player location and world-facing gameplay state.
- Death never reloads a save.  The world continues while the server records a
  dead player and later issues one authoritative respawn transition.
- Solo respawns use the last safe authoritative player context.  Party
  respawns use a safe location in the party's current cell.
- Normal exterior/interior transitions use Skyrim streaming and a server
  context handoff; they must not save-load or rewind a client.

## Quest policy

- Skyrim's scripted main-story progression is disabled in the MP branch.
- Closed alpha quests are server-approved radiant, exploration, bounty,
  dungeon, and server-authored party objectives.
- Campaign quest mods require a server quest program or an explicit adapter
  for every gameplay-critical scripted effect.

## Mod policy

- Pure presentation mods may be optional when they do not alter records used
  by gameplay or world placement.
- World, quest, item, combat, NPC, or navigation mods require the exact
  approved version, dependency set, hashes, and load order on the server and
  every client.
- The launcher will compare a server manifest before joining.  It may offer
  an authorized download source, but must not redistribute a Nexus file whose
  permissions do not allow it.
