# IPC Rich Presence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the broken WebSocket-based Rich Presence with a proper Discord IPC named pipe connection, fixing Linux compatibility and making the App ID override field functional.

**Architecture:** A new `DiscordIPC` class manages a `\\.\pipe\discord-ipc-N` connection on a background thread, handles the IPC frame protocol (opcode + length + JSON), and exposes the same `SetActivity`/`ClearActivity` interface. `RichPresence` is rewired to use it. The voice overlay `DiscordClient` is cleaned up by removing its now-dead activity code. On Linux, users need `wine-discord-ipc-bridge` running alongside GW2.

**Tech Stack:** C++17, Windows named pipes (CreateFileW/WriteFile/ReadFile), nlohmann/json, MinGW cross-compile.

**Linux prerequisite:** `wine-discord-ipc-bridge` (AUR: `wine-discord-ipc-bridge`) must be running. It bridges `\\.\pipe\discord-ipc-N` inside Wine to `$XDG_RUNTIME_DIR/discord-ipc-N` on the host.

---

## IPC Protocol Reference

Frame format: `[opcode: uint32_t LE][length: uint32_t LE][payload: UTF-8 JSON]`

Opcodes: 0=HANDSHAKE, 1=FRAME, 2=CLOSE

Connection sequence:
1. Open `\\.\pipe\discord-ipc-0` (try 0–9)
2. Send HANDSHAKE (opcode=0): `{"v":1,"client_id":"APP_ID"}`
3. Read response frame — expect `{"cmd":"DISPATCH","evt":"READY",...}`
4. Send SET_ACTIVITY frames (opcode=1) — same JSON structure already used by RichPresence

---

## File Map

**Create:**
- `src/DiscordIPC.h`
- `src/DiscordIPC.cpp`

**Modify:**
- `src/RichPresence.h` — change `DiscordClient*` to `DiscordIPC*`
- `src/RichPresence.cpp` — update include and constructor
- `src/DiscordClient.h` — remove SetActivity, ClearActivity, activity queue members
- `src/DiscordClient.cpp` — remove SetActivity, ClearActivity, activity send in network loop, revert scope
- `src/dllmain.cpp` — add DiscordIPC lifecycle, fix App ID wiring
- `CMakeLists.txt` — add DiscordIPC.cpp to sources

---

### Task 1: Create DiscordIPC class

**Files:**
- Create: `src/DiscordIPC.h`
- Create: `src/DiscordIPC.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create src/DiscordIPC.h**

```cpp
#pragma once
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <windows.h>

class DiscordIPC {
public:
    DiscordIPC();
    ~DiscordIPC();

    // Start background thread with the given Discord Application ID.
    void Start(const std::string& appId);

    // Stop background thread and close pipe.
    void Stop();

    // Change the App ID and reconnect. Safe to call from any thread.
    void SetAppId(const std::string& appId);

    // Queue a Rich Presence activity update. Thread-safe.
    // activityJson must be a complete Discord RPC envelope:
    //   {"cmd":"SET_ACTIVITY","nonce":"...","args":{"pid":...,"activity":{...}}}
    void SetActivity(const std::string& activityJson);

    // Queue a presence clear. Thread-safe.
    void ClearActivity();

private:
    void ThreadMain();
    bool Connect();
    void Disconnect();
    bool SendFrame(uint32_t opcode, const std::string& payload);
    bool ReadFrame(uint32_t& opcode, std::string& payload);

    HANDLE              m_pipe = INVALID_HANDLE_VALUE;
    std::string         m_appId;
    std::mutex          m_appIdMutex;
    std::atomic<bool>   m_reconnect{false};

    std::thread         m_thread;
    std::atomic<bool>   m_running{false};

    std::mutex          m_activityMutex;
    std::string         m_pendingActivity;
    std::atomic<bool>   m_activityPending{false};

    uint64_t            m_reconnectAt = 0;
};
```

- [ ] **Step 2: Create src/DiscordIPC.cpp**

```cpp
#include "DiscordIPC.h"
#include <nlohmann/json.hpp>
#include <string>
#include <cstdint>

using json = nlohmann::json;

static constexpr uint32_t OP_HANDSHAKE = 0;
static constexpr uint32_t OP_FRAME     = 1;

DiscordIPC::DiscordIPC() {}

DiscordIPC::~DiscordIPC() {
    Stop();
}

void DiscordIPC::Start(const std::string& appId) {
    {
        std::lock_guard<std::mutex> lock(m_appIdMutex);
        m_appId = appId;
    }
    m_running.store(true);
    m_thread = std::thread(&DiscordIPC::ThreadMain, this);
}

void DiscordIPC::Stop() {
    m_running.store(false);
    if (m_thread.joinable())
        m_thread.join();
    Disconnect();
}

void DiscordIPC::SetAppId(const std::string& appId) {
    std::lock_guard<std::mutex> lock(m_appIdMutex);
    if (m_appId != appId) {
        m_appId = appId;
        m_reconnect.store(true);
    }
}

void DiscordIPC::SetActivity(const std::string& activityJson) {
    std::lock_guard<std::mutex> lock(m_activityMutex);
    m_pendingActivity = activityJson;
    m_activityPending.store(true);
}

void DiscordIPC::ClearActivity() {
    json j;
    j["cmd"]              = "SET_ACTIVITY";
    j["nonce"]            = "rp_clear";
    j["args"]["pid"]      = (int)GetCurrentProcessId();
    j["args"]["activity"] = nullptr;
    SetActivity(j.dump());
}

bool DiscordIPC::SendFrame(uint32_t opcode, const std::string& payload) {
    if (m_pipe == INVALID_HANDLE_VALUE) return false;
    uint32_t len = (uint32_t)payload.size();
    // Write header
    DWORD written = 0;
    uint32_t header[2] = {opcode, len};
    if (!WriteFile(m_pipe, header, sizeof(header), &written, nullptr) || written != sizeof(header))
        return false;
    // Write payload
    if (len > 0) {
        if (!WriteFile(m_pipe, payload.data(), len, &written, nullptr) || written != len)
            return false;
    }
    return true;
}

bool DiscordIPC::ReadFrame(uint32_t& opcode, std::string& payload) {
    if (m_pipe == INVALID_HANDLE_VALUE) return false;
    // Peek to see if data is available (non-blocking check)
    DWORD available = 0;
    if (!PeekNamedPipe(m_pipe, nullptr, 0, nullptr, &available, nullptr) || available < 8)
        return false;
    uint32_t header[2] = {0, 0};
    DWORD read = 0;
    if (!ReadFile(m_pipe, header, sizeof(header), &read, nullptr) || read != sizeof(header))
        return false;
    opcode = header[0];
    uint32_t len = header[1];
    payload.resize(len);
    if (len > 0) {
        if (!ReadFile(m_pipe, payload.data(), len, &read, nullptr) || read != len)
            return false;
    }
    return true;
}

bool DiscordIPC::Connect() {
    std::string appId;
    {
        std::lock_guard<std::mutex> lock(m_appIdMutex);
        appId = m_appId;
    }
    if (appId.empty()) return false;

    wchar_t pipeName[] = L"\\\\.\\pipe\\discord-ipc-0";
    const size_t digitIdx = (sizeof(pipeName) / sizeof(wchar_t)) - 2; // index of '0' before null
    for (int i = 0; i < 10 && m_running.load(); ++i) {
        pipeName[digitIdx] = L'0' + i;
        m_pipe = CreateFileW(pipeName, GENERIC_READ | GENERIC_WRITE,
                             0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (m_pipe != INVALID_HANDLE_VALUE) break;
    }
    if (m_pipe == INVALID_HANDLE_VALUE) return false;

    // Send handshake
    json hs;
    hs["v"]         = 1;
    hs["client_id"] = appId;
    if (!SendFrame(OP_HANDSHAKE, hs.dump())) {
        Disconnect();
        return false;
    }

    // Wait briefly for READY response
    Sleep(200);
    uint32_t op = 0;
    std::string resp;
    if (!ReadFrame(op, resp)) {
        // No response yet — optimistically continue; Discord sends READY asynchronously
        return true;
    }
    try {
        auto rj = json::parse(resp);
        if (rj.value("evt", "") != "READY") {
            Disconnect();
            return false;
        }
    } catch (...) {
        Disconnect();
        return false;
    }
    return true;
}

void DiscordIPC::Disconnect() {
    if (m_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }
}

void DiscordIPC::ThreadMain() {
    while (m_running.load()) {
        // Handle App ID change — disconnect so next iteration reconnects
        if (m_reconnect.load()) {
            m_reconnect.store(false);
            Disconnect();
            m_reconnectAt = 0;
        }

        // Reconnect if needed
        if (m_pipe == INVALID_HANDLE_VALUE) {
            uint64_t now = GetTickCount64();
            if (m_reconnectAt > 0 && now < m_reconnectAt) {
                Sleep(500);
                continue;
            }
            if (!Connect()) {
                m_reconnectAt = GetTickCount64() + 10000;
                Sleep(500);
                continue;
            }
            // On fresh connect, re-send last known activity if one is queued
        }

        // Drain incoming frames (ping/close/etc)
        uint32_t op = 0;
        std::string payload;
        while (ReadFrame(op, payload)) {
            if (op == 2) { // CLOSE
                Disconnect();
                m_reconnectAt = GetTickCount64() + 5000;
                break;
            }
        }

        // Check pipe is still alive
        if (m_pipe != INVALID_HANDLE_VALUE) {
            DWORD flags = 0;
            if (!GetNamedPipeHandleStateA(m_pipe, &flags, nullptr, nullptr, nullptr, nullptr, 0)) {
                Disconnect();
                m_reconnectAt = GetTickCount64() + 5000;
            }
        }

        // Send pending activity
        if (m_activityPending.load() && m_pipe != INVALID_HANDLE_VALUE) {
            std::string act;
            {
                std::lock_guard<std::mutex> lock(m_activityMutex);
                act = m_pendingActivity;
                m_pendingActivity.clear();
                m_activityPending.store(false);
            }
            if (!SendFrame(OP_FRAME, act)) {
                Disconnect();
                m_reconnectAt = GetTickCount64() + 5000;
            }
        }

        Sleep(16);
    }

    Disconnect();
}
```

- [ ] **Step 3: Add DiscordIPC.cpp to CMakeLists.txt**

Find the `set(SOURCES` block and add `src/DiscordIPC.cpp` to it, alongside the other source files.

- [ ] **Step 4: Build**

```bash
cd /home/tony/Dev/discordant/build && make 2>&1 | tail -5
```

Expected: success.

- [ ] **Step 5: Commit**

```bash
git add src/DiscordIPC.h src/DiscordIPC.cpp CMakeLists.txt
git commit -m "feat: add DiscordIPC class for named pipe Rich Presence"
```

---

### Task 2: Rewire RichPresence to use DiscordIPC

**Files:**
- Modify: `src/RichPresence.h`
- Modify: `src/RichPresence.cpp`

- [ ] **Step 1: Update RichPresence.h**

Replace the forward declaration and member:

```cpp
// Change:
class DiscordClient;
// To:
class DiscordIPC;
```

Change the constructor declaration:
```cpp
// Change:
explicit RichPresence(DiscordClient* client);
// To:
explicit RichPresence(DiscordIPC* ipc);
```

Change the private member:
```cpp
// Change:
DiscordClient* m_client;
// To:
DiscordIPC* m_ipc;
```

- [ ] **Step 2: Update RichPresence.cpp**

Replace the include:
```cpp
// Change:
#include "DiscordClient.h"
// To:
#include "DiscordIPC.h"
```

Replace the constructor:
```cpp
// Change:
RichPresence::RichPresence(DiscordClient* client)
    : m_client(client)
{}
// To:
RichPresence::RichPresence(DiscordIPC* ipc)
    : m_ipc(ipc)
{}
```

Replace all four calls to `m_client->` with `m_ipc->`:
- `m_client->ClearActivity()` → `m_ipc->ClearActivity()`  (2 occurrences)
- `m_client->SetActivity(...)` → `m_ipc->SetActivity(...)`  (1 occurrence)

- [ ] **Step 3: Build**

```bash
cd /home/tony/Dev/discordant/build && make 2>&1 | tail -5
```

Expected: success.

- [ ] **Step 4: Commit**

```bash
git add src/RichPresence.h src/RichPresence.cpp
git commit -m "refactor: RichPresence now uses DiscordIPC instead of DiscordClient"
```

---

### Task 3: Clean up DiscordClient

**Files:**
- Modify: `src/DiscordClient.h`
- Modify: `src/DiscordClient.cpp`

Remove all activity-related code that was added for the now-replaced WebSocket approach.

- [ ] **Step 1: Remove from DiscordClient.h**

Remove the two public method declarations:
```cpp
void SetActivity(const std::string& activityJson);
void ClearActivity();
```

Remove the three private members:
```cpp
std::mutex m_activityMutex;
std::string m_pendingActivity;
std::atomic<bool> m_activityPending{false};
```

Also remove the comment block above them:
```cpp
// Pending Rich Presence activity (written by render thread, sent by network thread)
```

- [ ] **Step 2: Remove from DiscordClient.cpp**

Remove the `SetActivity` and `ClearActivity` implementations (the two functions after `DrainLogQueue`).

Remove the pending activity send block from `NetworkThreadMain` — the block that reads:
```cpp
// Send queued Rich Presence activity update
if (m_activityPending.load() && m_state.load() == DiscordState::Connected) {
    ...
}
```

- [ ] **Step 3: Revert AUTHORIZE scope**

In `StartAuthorize()`, change:
```cpp
args["scopes"] = json::array({"rpc", "rpc.activities.write"});
```
back to:
```cpp
args["scopes"] = json::array({"rpc"});
```

This also means the next time users connect, they will be re-prompted by Discord to authorise (since the scope list changed again). This is unavoidable — the token from the previous scope set will fail, and the error handler will call `StartAuthorize()` automatically.

- [ ] **Step 4: Build**

```bash
cd /home/tony/Dev/discordant/build && make 2>&1 | tail -5
```

Expected: success.

- [ ] **Step 5: Commit**

```bash
git add src/DiscordClient.h src/DiscordClient.cpp
git commit -m "refactor: remove WebSocket activity code from DiscordClient; revert scope"
```

---

### Task 4: Update dllmain

**Files:**
- Modify: `src/dllmain.cpp`

- [ ] **Step 1: Add DiscordIPC include**

After `#include "RichPresence.h"`, add:
```cpp
#include "DiscordIPC.h"
```

- [ ] **Step 2: Add DiscordIPC global**

After `static RichPresence* g_RichPresence = nullptr;`, add:
```cpp
static DiscordIPC* g_DiscordIPC = nullptr;
```

- [ ] **Step 3: Add helper to resolve effective App ID**

Before `AddonLoad`, add:
```cpp
static std::string GetEffectiveAppId() {
    if (g_RPAppIdOverride[0] != '\0')
        return std::string(g_RPAppIdOverride);
    return DISCORD_APP_ID;
}
```

- [ ] **Step 4: Update AddonLoad**

Replace:
```cpp
g_RichPresence = new RichPresence(g_Discord);
```
with:
```cpp
g_DiscordIPC = new DiscordIPC();
g_DiscordIPC->Start(GetEffectiveAppId());
g_RichPresence = new RichPresence(g_DiscordIPC);
```

- [ ] **Step 5: Update AddonUnload**

After the existing `Events_Unsubscribe` / MumbleLink teardown block, add DiscordIPC teardown **before** the RichPresence block:

The unload order should be:
1. Unsubscribe Mumble event, null MumbleLink
2. Shutdown + delete RichPresence
3. Stop + delete DiscordIPC  ← add this
4. Disconnect + delete DiscordClient (unchanged)

Add after deleting `g_RichPresence`:
```cpp
    if (g_DiscordIPC) {
        g_DiscordIPC->Stop();
        delete g_DiscordIPC;
        g_DiscordIPC = nullptr;
    }
```

- [ ] **Step 6: Propagate App ID changes to DiscordIPC**

In `SaveConfig()`, after the `g_RPAppIdOverride` save line, add:
```cpp
        if (g_DiscordIPC)
            g_DiscordIPC->SetAppId(GetEffectiveAppId());
```

This ensures that when the user edits the App ID field and it auto-saves, the IPC connection reconnects with the new ID.

- [ ] **Step 7: Update the App ID tooltip**

Find the `ImGui::SetTooltip` for the Application ID field and change the text from:
```
"Custom application ID support is reserved for a future update."
```
to:
```
"Your Discord Application ID. Leave blank to use the built-in ID. Changes take effect immediately."
```

- [ ] **Step 8: Build**

```bash
cd /home/tony/Dev/discordant/build && make 2>&1 | tail -5
```

Expected: success.

- [ ] **Step 9: Commit**

```bash
git add src/dllmain.cpp
git commit -m "feat: wire DiscordIPC into addon lifecycle; App ID override now functional"
```

---

## Post-Implementation Testing Checklist

**Linux (requires wine-discord-ipc-bridge running):**
- [ ] `wine-discord-ipc-bridge` started before GW2
- [ ] Enable Rich Presence in options → activity appears on Discord profile
- [ ] Custom App ID entered → IPC reconnects and shows under that app
- [ ] Map changes update the presence
- [ ] Loading screen shows "Playing Guild Wars 2"
- [ ] Disable Rich Presence → activity cleared from Discord
- [ ] Addon hot-reload doesn't crash

**Windows:**
- [ ] Same checklist without the wine-discord-ipc-bridge step
