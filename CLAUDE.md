# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

Discordant is a Guild Wars 2 Nexus addon (Windows DLL) that shows Discord voice channel participants as an in-game overlay. It connects to the locally running Discord desktop client via its WebSocket RPC server on `127.0.0.1:6463-6472`, using Discord's StreamKit OAuth flow.

## Build

**Prerequisites:** CMake 3.20+, MinGW cross-compiler (`x86_64-w64-mingw32-g++`)

```bash
# First time: download ImGui and nlohmann/json
./scripts/setup.sh

# Build
mkdir build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=../mingw-toolchain.cmake ..
make
```

Output: `build/Discordant.dll`

There are no tests. Build success is the only automated check. The user will test and report any issues.

## Architecture

This is a single-DLL addon with three layers:

**Nexus integration** (`dllmain.cpp`) — implements the Nexus addon API (`GetAddonDef`, `AddonLoad`, `AddonUnload`, `AddonRender`, `AddonOptions`). All ImGui rendering happens in `AddonRender` (called every frame by Nexus) and `AddonOptions` (Nexus settings panel). Config is persisted to `<addon_dir>/config.json` via nlohmann/json.

**Discord RPC client** (`DiscordClient.cpp/.h`) — runs a background networking thread (`NetworkThreadMain`) that manages the WebSocket connection, handles the StreamKit OAuth flow (Authorize → ExchangeToken → Authenticate), subscribes to Discord voice events, and maintains `m_users` (the voice user list) under `m_userMutex`. The render thread calls `GetVoiceSnapshot()` to get a thread-safe copy. Logs are queued in `m_logQueue` and drained by the render thread every 30 frames to forward to the Nexus logger.

**WebSocket client** (`WebSocket.cpp/.h`) — minimal RFC 6455 implementation over Winsock2, with no external dependencies.

## Key Details

- **Thread safety:** The network thread writes to `m_users`/`m_channelName` etc. under `m_userMutex`. The render thread must always go through `GetVoiceSnapshot()`, never access members directly.
- **Token caching:** The Discord OAuth access token is saved to `config.json` and reloaded on startup. `SetAccessToken` / `GetAccessToken` are the interface.
- **Avatar textures:** Loaded asynchronously via `APIDefs->Textures_GetOrCreateFromURL`. The `s_avatarCache` map in `dllmain.cpp` tracks in-flight and completed requests to avoid duplicate calls.
- **DllMain constraint:** No C++ objects with non-trivial constructors/destructors as globals. `DiscordClient` is heap-allocated in `AddonLoad` and deleted in `AddonUnload`.
- **Version:** Defined as `V_MAJOR/V_MINOR/V_BUILD/V_REVISION` constants at the top of `dllmain.cpp`. Update these before releasing.
- **Releases:** Always build the DLL first and attach `build/Discordant.dll` to the GitHub release as a binary asset.
- **Nexus API:** The full API surface is in `include/nexus/Nexus.h`.
