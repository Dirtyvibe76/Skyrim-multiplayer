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
