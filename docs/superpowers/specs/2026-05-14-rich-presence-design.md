# Rich Presence — Design Spec

**Date:** 2026-05-14
**Project:** Discordant (GW2 Nexus addon)

## Overview

Add Discord Rich Presence support to Discordant, showing the player's in-game status (character name, current map, profession, party size) on their Discord profile. Uses the existing WebSocket RPC connection rather than the Discord IPC named pipe, avoiding Wine/Linux compatibility issues.

## Architecture

### New files

- `src/RichPresence.h` / `src/RichPresence.cpp` — reads MumbleLink via the Nexus API, builds the activity JSON payload, calls `DiscordClient::SetActivity` / `ClearActivity` when state changes.
- `src/MapData.h` — bundled map ID → name lookup table, copied and maintained independently from `say_again`. Must include guild hall instances and Eternity's Garden (map ID to be looked up during implementation).
- `cmake/secrets.cmake` (gitignored) — defines `DISCORD_APP_ID` build variable.
- `cmake/secrets.cmake.example` (committed) — documents the format.

### Changes to existing files

- `src/DiscordClient.h` / `.cpp` — add `SetActivity(const nlohmann::json&)` and `ClearActivity()` public methods. These format and send the appropriate RPC frame over the existing WebSocket connection.
- `src/dllmain.cpp` — heap-allocate a `RichPresence` instance in `AddonLoad`, delete in `AddonUnload`, call `RichPresence::Update()` from `AddonRender`, add Rich Presence section to `AddonOptions`.
- `CMakeLists.txt` — include `cmake/secrets.cmake`, pass `DISCORD_APP_ID` as a compile definition.

### OAuth scope

Add `rpc.activities.write` to the existing scope request. Existing cached tokens will be invalidated; users re-authorise once.

### Discord Application ID

Injected at build time via `cmake/secrets.cmake`. Users may override with their own Application ID in the options pane; changing it triggers a re-handshake.

## Data

MumbleLink fields used:

| Field | Discord placement |
|---|---|
| Character name | Details line (top) |
| Map name (resolved from map ID) | State line (bottom) |
| Profession | Large image key + tooltip |
| Party size | Party current/max |

Map name is resolved via the bundled `MapData.h` lookup. Unknown map IDs fall back to `"Unknown Map"`.

## UI — Options Pane

New "Rich Presence" section in `AddonOptions`:

```
[ ] Enable Rich Presence
    Application ID: [________________]  (leave blank to use default)
    [ ] Show character name
    [ ] Show current map
    [ ] Show profession
    [ ] Show party size
```

Sub-checkboxes and the Application ID field are disabled when the master checkbox is off. All values persisted to `config.json`.

## Update Logic

`RichPresence::Update()` is called every frame but caches the last-sent state. `SetActivity` is only called when character name, map ID, party size, or profession changes — keeping Discord traffic minimal.

## Edge Cases

| State | Behaviour |
|---|---|
| In-game, all data fields unchecked | Details: `Playing Guild Wars 2`, no state line |
| Character select / loading screen (map ID 0 or empty name) | Details: `Playing Guild Wars 2`, no state line |
| Rich Presence disabled by user | `ClearActivity()` called immediately |
| WebSocket disconnected | `SetActivity` is a no-op; re-sends on reconnect |
| Map ID not in lookup table | State line shows `Unknown Map` |
| `rpc.activities.write` scope rejected | Presence silently fails; voice overlay unaffected |

## Out of Scope

- Animated avatar support
- Automatic map table updates from the GW2 API
- Per-character or per-account presence customisation
