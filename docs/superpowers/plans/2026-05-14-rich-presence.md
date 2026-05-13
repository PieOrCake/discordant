# Rich Presence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend Discordant to send GW2 in-game status (character name, map, profession) to the user's Discord profile via the existing WebSocket RPC connection.

**Architecture:** `RichPresence` reads `MumbleIdentity` from a dllmain global updated by an event callback, detects state changes, and calls `DiscordClient::SetActivity` / `ClearActivity`. These methods queue a pending payload that the network thread sends over the already-open WebSocket on its next iteration.

**Tech Stack:** C++17, nlohmann/json, ImGui, Nexus API (MumbleLink / DataLink / Events), Windows (MinGW cross-compile).

> **Note on DISCORD_APP_ID:** The App ID is configured in Task 1 but not yet used in the RPC payload — the activity is sent through the existing StreamKit connection, so Discord will attribute presence to the StreamKit app. A future iteration will open a second connection using `DISCORD_APP_ID` as the `client_id` for proper GW2 app branding. For now the feature is fully functional, just without custom app name/images.

> **Note on party size:** `MumbleIdentity` does not expose party size. The UI toggle is present but always sends no party data. Future iteration required if a data source is found.

---

## File Map

**Create:**
- `src/RichPresence.h`
- `src/RichPresence.cpp`
- `src/MapData.h`
- `cmake/secrets.cmake.example`

**Modify:**
- `src/DiscordClient.h` — add `SetActivity`, `ClearActivity`, pending activity members
- `src/DiscordClient.cpp` — implement activity methods, add scope, send in network loop
- `src/dllmain.cpp` — add MumbleLink globals, RP config, options UI, lifecycle wiring
- `CMakeLists.txt` — include secrets.cmake, add compile definition
- `.gitignore` — ignore cmake/secrets.cmake

---

### Task 1: CMake secrets setup

**Files:**
- Create: `cmake/secrets.cmake.example`
- Create: `cmake/secrets.cmake` (gitignored — developer creates this)
- Modify: `CMakeLists.txt`
- Modify: `.gitignore`

- [ ] **Step 1: Create cmake/secrets.cmake.example**

```cmake
# Copy this file to cmake/secrets.cmake and fill in your Discord Application ID.
# cmake/secrets.cmake is gitignored — never commit your real App ID.
set(DISCORD_APP_ID "YOUR_APP_ID_HERE")
```

- [ ] **Step 2: Create cmake/secrets.cmake with the real App ID**

```cmake
set(DISCORD_APP_ID "1504236832525783170")
```

- [ ] **Step 3: Update .gitignore**

Add to the end of `.gitignore`:

```
# Secrets
cmake/secrets.cmake
```

- [ ] **Step 4: Update CMakeLists.txt**

After the `project(Discordant)` line, add:

```cmake
# Load developer secrets (App IDs, etc.) — file is gitignored
include(${CMAKE_SOURCE_DIR}/cmake/secrets.cmake OPTIONAL)
if(NOT DEFINED DISCORD_APP_ID)
    set(DISCORD_APP_ID "")
endif()
```

After `add_library(Discordant SHARED ...)`, add:

```cmake
target_compile_definitions(Discordant PRIVATE
    DISCORD_APP_ID="${DISCORD_APP_ID}"
)
```

- [ ] **Step 5: Build to confirm CMake changes are valid**

```bash
cd /home/tony/Dev/discordant/build && cmake .. -DCMAKE_TOOLCHAIN_FILE=../mingw-toolchain.cmake && make 2>&1 | tail -5
```

Expected: no errors, `Discordant.dll` emitted.

- [ ] **Step 6: Commit**

```bash
git add cmake/secrets.cmake.example CMakeLists.txt .gitignore
git commit -m "build: add cmake secrets infrastructure for Discord App ID"
```

---

### Task 2: MapData.h

**Files:**
- Create: `src/MapData.h`

- [ ] **Step 1: Create src/MapData.h**

Copy the full content from `say_again`, then add a `"Guild Halls"` group and `Eternity's Garden` before closing the vector. Look up the map IDs from the GW2 API (`https://api.guildwars2.com/v2/maps?ids=all`) and replace the placeholder comments. Eternity's Garden was released 2026-05-13.

```cpp
#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace MapData {

struct MapEntry {
    uint32_t    id;
    const char* name;
};

struct MapGroup {
    const char*           groupName;
    std::vector<MapEntry> maps;
};

inline const std::vector<MapGroup>& GetMapGroups() {
    static const std::vector<MapGroup> kGroups = {
        { "Core Tyria", {
            { 15u,   "Queensdale" },
            { 17u,   "Harathi Hinterlands" },
            { 18u,   "Divinity's Reach" },
            { 23u,   "Kessex Hills" },
            { 24u,   "Gendarran Fields" },
            { 50u,   "Lion's Arch" },
            { 73u,   "Bloodtide Coast" },
            { 335u,  "Claw Island" },
            { 873u,  "Southsun Cove" },
            { 929u,  "The Crown Pavilion" },
            { 1155u, "Lion's Arch Aerodrome" },
            { 1483u, "Memory of Old Lion's Arch" },
            { 19u,   "Plains of Ashford" },
            { 20u,   "Blazeridge Steppes" },
            { 21u,   "Fields of Ruin" },
            { 22u,   "Fireheart Rise" },
            { 25u,   "Iron Marches" },
            { 32u,   "Diessa Plateau" },
            { 218u,  "Black Citadel" },
            { 26u,   "Dredgehaunt Cliffs" },
            { 27u,   "Lornar's Pass" },
            { 28u,   "Wayfarer Foothills" },
            { 29u,   "Timberline Falls" },
            { 30u,   "Frostgorge Sound" },
            { 31u,   "Snowden Drifts" },
            { 326u,  "Hoelbrak" },
            { 51u,   "Straits of Devastation" },
            { 62u,   "Cursed Shore" },
            { 65u,   "Malchor's Leap" },
            { 34u,   "Caledon Forest" },
            { 35u,   "Metrica Province" },
            { 39u,   "Mount Maelstrom" },
            { 53u,   "Sparkfly Fen" },
            { 54u,   "Brisban Wildlands" },
            { 91u,   "The Grove" },
            { 139u,  "Rata Sum" },
            { 922u,  "Labyrinthine Cliffs" },
        }},
        { "Living World Season 2", {
            { 988u,  "Dry Top" },
            { 1015u, "The Silverwastes" },
        }},
        { "Heart of Thorns", {
            { 1042u, "Verdant Brink" },
            { 1052u, "Verdant Brink" },
            { 1043u, "Auric Basin" },
            { 1045u, "Tangled Depths" },
            { 1041u, "Dragon's Stand" },
            { 1095u, "Dragon's Stand (Heart of Thorns)" },
            { 1158u, "Noble's Folly" },
        }},
        { "Living World Season 3", {
            { 1165u, "Bloodstone Fen" },
            { 1175u, "Ember Bay" },
            { 1178u, "Bitterfrost Frontier" },
            { 1185u, "Lake Doric" },
            { 1195u, "Draconis Mons" },
            { 1203u, "Siren's Landing" },
        }},
        { "Path of Fire", {
            { 1210u, "Crystal Oasis" },
            { 1211u, "Desert Highlands" },
            { 1215u, "Windswept Haven" },
            { 1226u, "The Desolation" },
            { 1228u, "Elon Riverlands" },
            { 1248u, "Domain of Vabbi" },
        }},
        { "Living World Season 4", {
            { 1263u, "Domain of Istan" },
            { 1271u, "Sandswept Isles" },
            { 1288u, "Domain of Kourna" },
            { 1301u, "Jahai Bluffs" },
            { 1310u, "Thunderhead Peaks" },
            { 1317u, "Dragonfall" },
        }},
        { "Icebrood Saga", {
            { 1330u, "Grothmar Valley" },
            { 1343u, "Bjora Marches" },
            { 1370u, "Eye of the North" },
            { 1371u, "Drizzlewood Coast" },
        }},
        { "End of Dragons", {
            { 1422u, "Dragon's End" },
            { 1428u, "Arborstone" },
            { 1438u, "New Kaineng City" },
            { 1442u, "Seitung Province" },
            { 1452u, "The Echovald Wilds" },
            { 1465u, "Thousand Seas Pavilion" },
            { 1490u, "Gyala Delve" },
        }},
        { "Secrets of the Obscure", {
            { 1509u, "The Wizard's Tower" },
            { 1510u, "Skywatch Archipelago" },
            { 1517u, "Amnytas" },
            { 1526u, "Inner Nayos" },
            { 1593u, "Starlit Weald" },
            { 1595u, "Shipwreck Strand" },
            { 1609u, "Guardian's Glade" },
            { 1596u, "Comosus Isle" },
        }},
        { "Janthir Wilds", {
            { 1550u, "Lowland Shore" },
            { 1554u, "Janthir Syntri" },
            { 1574u, "Bava Nisos" },
            { 1575u, "Mistburned Barrens" },
            { 1558u, "Hearth's Glow" },
            { 1557u, "Abandoned Homestead" },
        }},
        { "New Maps", {
            // Eternity's Garden (released 2026-05-13) — look up map ID from GW2 API
            // { XXXX, "Eternity's Garden" },
        }},
        { "Guild Halls", {
            // Look up IDs from GW2 API: https://api.guildwars2.com/v2/maps?ids=all
            // Search for "Gilded Hollow", "Lost Precipice", "Windswept Haven",
            // "Isle of Reflection", "Armistice Bastion"
            // { XXXX, "Gilded Hollow" },
            // { XXXX, "Lost Precipice" },
            // { XXXX, "Isle of Reflection" },
            // { XXXX, "Armistice Bastion" },
        }},
        { "WvW", {
            { 38u,   "Eternal Battlegrounds" },
            { 95u,   "Alpine Borderlands" },
            { 96u,   "Alpine Borderlands" },
            { 968u,  "Edge of the Mists" },
            { 1099u, "Desert Borderlands" },
        }},
        { "Convergences", {
            { 1523u, "Convergence: Outer Nayos (Public)" },
            { 1527u, "Convergence: Outer Nayos (Private Squad)" },
            { 1562u, "Convergence: Mount Balrior (Private Squad)" },
            { 1571u, "Convergence: Mount Balrior (Public)" },
        }},
        { "Raids", {
            { 1062u, "Spirit Vale" },
            { 1149u, "Salvation Pass" },
            { 1156u, "Stronghold of the Faithful" },
            { 1188u, "Bastion of the Penitent" },
            { 1264u, "Hall of Chains" },
            { 1303u, "Mythwright Gambit" },
            { 1323u, "The Key of Ahdashim" },
            { 1504u, "Bastion of the Obscure" },
            { 1507u, "Bastion of Strength" },
            { 1512u, "Bastion of the Celestial" },
        }},
        { "Strikes", {
            { 1332u, "Shiverpeaks Pass" },
            { 1339u, "Boneskinner" },
            { 1341u, "Fraenir of Jormag" },
            { 1346u, "Voice of the Fallen and Claw of the Fallen" },
            { 1359u, "Whisper of Jormag" },
            { 1368u, "Forging Steel" },
            { 1374u, "Cold War" },
            { 1409u, "Dragonstorm (Private Squad)" },
            { 1412u, "Dragonstorm" },
            { 1414u, "The Twisted Marionette (Private Squad)" },
            { 1480u, "The Twisted Marionette" },
            { 1432u, "Aetherblade Hideout" },
            { 1437u, "Harvest Temple" },
            { 1450u, "Xunlai Jade Junkyard" },
            { 1451u, "Kaineng Overlook" },
            { 1485u, "Old Lion's Court" },
            { 1515u, "Cosmic Observatory" },
            { 1520u, "Temple of Febe" },
            { 1564u, "Mount Balrior" },
            { 1567u, "Harvest Den" },
            { 1572u, "Balrior Peak: Mount Balrior" },
            { 1583u, "Salvation's Cost: Foundry of Failed Creations" },
            { 1585u, "Salvation's Cost: Saevus's Heart" },
        }},
        { "Mistlock Sanctuary", {
            { 1206u, "Mistlock Sanctuary" },
        }},
        { "Fractals of the Mists", {
            {  872u, "Fractals of the Mists" },
            {  947u, "Volcanic" },
            {  948u, "Uncategorized" },
            {  949u, "Ocean" },
            {  950u, "Swampland" },
            {  951u, "Urban Battleground" },
            {  952u, "Aquatic Ruins" },
            {  953u, "Cliffside" },
            {  954u, "Underground Facility" },
            {  955u, "Molten Furnace" },
            {  956u, "Aetherblade" },
            {  957u, "Thaumanova Reactor" },
            {  958u, "Solid Ocean" },
            {  959u, "Snowblind" },
            {  960u, "Molten Boss" },
            { 1164u, "Chaos" },
            { 1177u, "Nightmare" },
            { 1205u, "Shattered Observatory" },
            { 1267u, "Twilight Oasis" },
            { 1290u, "Deepstone" },
            { 1309u, "Siren's Reef" },
            { 1384u, "Sunqua Peak" },
            { 1500u, "Silent Surf" },
            { 1538u, "Lonely Tower" },
            { 1584u, "Kinfall" },
            { 1590u, "Fractal Incursion Conference" },
        }},
    };
    return kGroups;
}

inline const std::string& GetMapName(uint32_t mapId) {
    static const std::unordered_map<uint32_t, std::string> kFlat = []() {
        std::unordered_map<uint32_t, std::string> m;
        for (const auto& g : GetMapGroups())
            for (const auto& e : g.maps)
                m[e.id] = e.name;
        return m;
    }();
    static const std::string kUnknown = "Unknown Map";
    auto it = kFlat.find(mapId);
    return it != kFlat.end() ? it->second : kUnknown;
}

} // namespace MapData
```

- [ ] **Step 2: Build to confirm MapData.h compiles (it's header-only, so just ensure no syntax errors via the main build)**

```bash
cd /home/tony/Dev/discordant/build && make 2>&1 | tail -5
```

Expected: success, no errors.

- [ ] **Step 3: Commit**

```bash
git add src/MapData.h
git commit -m "feat: add bundled MapData.h with GW2 map ID lookup"
```

---

### Task 3: DiscordClient — activity queueing

**Files:**
- Modify: `src/DiscordClient.h`
- Modify: `src/DiscordClient.cpp`

The render thread calls `SetActivity`/`ClearActivity`. The network thread owns the WebSocket. To avoid concurrent writes, these methods store a pending payload under a mutex and set an atomic flag; the network thread sends it on its next iteration.

- [ ] **Step 1: Add pending activity members to DiscordClient.h**

In the `private:` section of `DiscordClient`, after the `m_logMutex` declaration, add:

```cpp
    // Pending Rich Presence activity (written by render thread, sent by network thread)
    std::mutex m_activityMutex;
    std::string m_pendingActivity;
    std::atomic<bool> m_activityPending{false};
```

- [ ] **Step 2: Declare SetActivity and ClearActivity in DiscordClient.h**

In the `public:` section, after `DrainLogQueue()`, add:

```cpp
    // Set or clear the Discord Rich Presence activity.
    // Thread-safe: queues payload for the network thread to send.
    void SetActivity(const std::string& activityJson);
    void ClearActivity();
```

- [ ] **Step 3: Implement SetActivity and ClearActivity in DiscordClient.cpp**

Add after `DrainLogQueue()`:

```cpp
void DiscordClient::SetActivity(const std::string& activityJson) {
    std::lock_guard<std::mutex> lock(m_activityMutex);
    m_pendingActivity = activityJson;
    m_activityPending.store(true);
}

void DiscordClient::ClearActivity() {
    nlohmann::json j;
    j["cmd"]  = "SET_ACTIVITY";
    j["nonce"] = "rp_clear";
    j["args"]["pid"] = (int)GetCurrentProcessId();
    j["args"]["activity"] = nullptr;
    SetActivity(j.dump());
}
```

- [ ] **Step 4: Send pending activity in NetworkThreadMain**

In `NetworkThreadMain`, after the closing `}` of the inner message-receive while loop (the one that calls `HandleMessage`) and before the `Sleep(16)` at the bottom of the outer while loop, add:

```cpp
        // Send queued Rich Presence activity update
        if (m_activityPending.load() && m_state.load() == DiscordState::Connected) {
            std::string act;
            {
                std::lock_guard<std::mutex> lock(m_activityMutex);
                act = m_pendingActivity;
                m_pendingActivity.clear();
            }
            m_activityPending.store(false);
            m_ws.SendText(act);
        }
```

- [ ] **Step 5: Add rpc.activities.write to AUTHORIZE scopes**

In `DiscordClient::StartAuthorize()`, change:

```cpp
    args["scopes"] = json::array({"rpc"});
```

to:

```cpp
    args["scopes"] = json::array({"rpc", "rpc.activities.write"});
```

Note: this invalidates any cached access token. Users will be prompted to re-authorise once.

- [ ] **Step 6: Clear the cached token when the scope list changes**

The existing token was issued for the old scope set and will fail authentication. In `DiscordClient::Authenticate()`, if the AUTHENTICATE response returns an ERROR event, `StartAuthorize()` is already called. No additional code needed — existing error handling covers this.

- [ ] **Step 7: Build**

```bash
cd /home/tony/Dev/discordant/build && make 2>&1 | tail -5
```

Expected: success.

- [ ] **Step 8: Commit**

```bash
git add src/DiscordClient.h src/DiscordClient.cpp
git commit -m "feat: add SetActivity/ClearActivity to DiscordClient; add rpc.activities.write scope"
```

---

### Task 4: RichPresence class

**Files:**
- Create: `src/RichPresence.h`
- Create: `src/RichPresence.cpp`

`RichPresence` is stateless except for caching the last-sent state to avoid redundant activity updates. It is told the current game state each frame via `Update()` and decides whether to call `SetActivity`.

- [ ] **Step 1: Create src/RichPresence.h**

```cpp
#pragma once
#include <string>
#include <cstdint>

class DiscordClient;

// Per-frame game state passed in from dllmain
struct RPGameState {
    bool        inGame;       // false = char select or loading screen
    std::string charName;
    uint32_t    mapId;
    unsigned    profession;   // 0=unknown, 1=Guardian ... 9=Revenant
};

// User configuration for what to include in the presence
struct RPConfig {
    bool enabled        = false;
    bool showCharName   = true;
    bool showMap        = true;
    bool showProfession = true;
    bool showPartySize  = false; // reserved — no data source yet
};

class RichPresence {
public:
    explicit RichPresence(DiscordClient* client);

    // Call once per frame from AddonRender. Sends activity only when state changes.
    void Update(const RPGameState& state, const RPConfig& cfg);

    // Call when the addon unloads to clear the activity.
    void Shutdown();

private:
    std::string BuildActivityJson(const RPGameState& state, const RPConfig& cfg) const;
    static const char* ProfessionName(unsigned prof);
    static const char* ProfessionKey(unsigned prof);  // Discord asset key

    DiscordClient* m_client;

    // Cached state to detect changes
    bool        m_lastEnabled    = false;
    bool        m_lastInGame     = false;
    std::string m_lastCharName;
    uint32_t    m_lastMapId      = 0;
    unsigned    m_lastProfession = 0;
    bool        m_lastShowName   = false;
    bool        m_lastShowMap    = false;
    bool        m_lastShowProf   = false;
};
```

- [ ] **Step 2: Create src/RichPresence.cpp**

```cpp
#include "RichPresence.h"
#include "DiscordClient.h"
#include "MapData.h"
#include <nlohmann/json.hpp>
#include <windows.h>
#include <string>

using json = nlohmann::json;

RichPresence::RichPresence(DiscordClient* client)
    : m_client(client)
{}

void RichPresence::Update(const RPGameState& state, const RPConfig& cfg) {
    // Detect any change that would alter the displayed presence
    bool changed =
        cfg.enabled        != m_lastEnabled    ||
        state.inGame       != m_lastInGame     ||
        state.charName     != m_lastCharName   ||
        state.mapId        != m_lastMapId      ||
        state.profession   != m_lastProfession ||
        cfg.showCharName   != m_lastShowName   ||
        cfg.showMap        != m_lastShowMap    ||
        cfg.showProfession != m_lastShowProf;

    if (!changed) return;

    m_lastEnabled    = cfg.enabled;
    m_lastInGame     = state.inGame;
    m_lastCharName   = state.charName;
    m_lastMapId      = state.mapId;
    m_lastProfession = state.profession;
    m_lastShowName   = cfg.showCharName;
    m_lastShowMap    = cfg.showMap;
    m_lastShowProf   = cfg.showProfession;

    if (!cfg.enabled) {
        m_client->ClearActivity();
        return;
    }

    m_client->SetActivity(BuildActivityJson(state, cfg));
}

void RichPresence::Shutdown() {
    if (m_lastEnabled)
        m_client->ClearActivity();
}

std::string RichPresence::BuildActivityJson(const RPGameState& state, const RPConfig& cfg) const {
    json activity;

    if (!state.inGame || (state.charName.empty() && state.mapId == 0)) {
        // Char select or loading screen
        activity["details"] = "Playing Guild Wars 2";
    } else {
        // Build details (top line) and state (bottom line)
        std::string details;
        std::string stateStr;

        if (cfg.showCharName && !state.charName.empty())
            details = state.charName;
        else
            details = "Playing Guild Wars 2";

        if (cfg.showMap && state.mapId != 0)
            stateStr = MapData::GetMapName(state.mapId);

        activity["details"] = details;
        if (!stateStr.empty())
            activity["state"] = stateStr;

        if (cfg.showProfession && state.profession > 0) {
            activity["assets"]["large_image"] = ProfessionKey(state.profession);
            activity["assets"]["large_text"]  = ProfessionName(state.profession);
        }
    }

    json j;
    j["cmd"]           = "SET_ACTIVITY";
    j["nonce"]         = "rp_set";
    j["args"]["pid"]   = (int)GetCurrentProcessId();
    j["args"]["activity"] = activity;
    return j.dump();
}

const char* RichPresence::ProfessionName(unsigned prof) {
    switch (prof) {
    case 1: return "Guardian";
    case 2: return "Warrior";
    case 3: return "Engineer";
    case 4: return "Ranger";
    case 5: return "Thief";
    case 6: return "Elementalist";
    case 7: return "Mesmer";
    case 8: return "Necromancer";
    case 9: return "Revenant";
    default: return "Unknown";
    }
}

const char* RichPresence::ProfessionKey(unsigned prof) {
    switch (prof) {
    case 1: return "guardian";
    case 2: return "warrior";
    case 3: return "engineer";
    case 4: return "ranger";
    case 5: return "thief";
    case 6: return "elementalist";
    case 7: return "mesmer";
    case 8: return "necromancer";
    case 9: return "revenant";
    default: return "gw2_logo";
    }
}
```

- [ ] **Step 3: Add RichPresence.cpp to CMakeLists.txt**

Find the `target_sources` or `add_library` line in `CMakeLists.txt` that lists source files and add `src/RichPresence.cpp`. The current source list line reads:

```cmake
add_library(Discordant SHARED
    src/dllmain.cpp
    src/DiscordClient.cpp
    src/WebSocket.cpp
    ...
```

Add `src/RichPresence.cpp` to that list.

- [ ] **Step 4: Build**

```bash
cd /home/tony/Dev/discordant/build && make 2>&1 | tail -5
```

Expected: success.

- [ ] **Step 5: Commit**

```bash
git add src/RichPresence.h src/RichPresence.cpp CMakeLists.txt
git commit -m "feat: add RichPresence class with change-driven activity updates"
```

---

### Task 5: dllmain integration

**Files:**
- Modify: `src/dllmain.cpp`

This task wires everything together: MumbleLink pointer, identity event callback, RP config globals, options UI, and the `RichPresence` instance lifecycle.

- [ ] **Step 1: Add includes and MumbleLink struct at the top of dllmain.cpp**

After the existing includes, add:

```cpp
#include "RichPresence.h"
```

After the existing global declarations (near `DiscordClient* g_Discord`), add:

```cpp
// MumbleLink
static Mumble::Data* g_MumbleLink = nullptr;

struct MumbleIdentity {
    char     Name[20];
    unsigned Profession;
    unsigned Specialization;
    unsigned Race;
    unsigned MapID;
    unsigned WorldID;
    unsigned TeamColorID;
    bool     IsCommander;
    float    FOV;
    unsigned UISize;
};
static MumbleIdentity g_MumbleIdentity{};

// Rich Presence
static RichPresence* g_RichPresence = nullptr;
static RPConfig      g_RPConfig{};
static char          g_RPAppIdOverride[64] = {0};  // user-supplied App ID (empty = use default)
```

- [ ] **Step 2: Add MumbleIdentity event callback**

Before `AddonLoad`, add:

```cpp
static void OnMumbleIdentityUpdated(void* eventArgs) {
    if (!eventArgs) return;
    g_MumbleIdentity = *reinterpret_cast<const MumbleIdentity*>(eventArgs);
}
```

- [ ] **Step 3: Update LoadConfig to read RP settings**

Inside the `try` block in `LoadConfig()`, after the existing `if (j.contains(...))` entries, add:

```cpp
            if (j.contains("rp_enabled"))      g_RPConfig.enabled      = j["rp_enabled"].get<bool>();
            if (j.contains("rp_show_name"))     g_RPConfig.showCharName = j["rp_show_name"].get<bool>();
            if (j.contains("rp_show_map"))      g_RPConfig.showMap      = j["rp_show_map"].get<bool>();
            if (j.contains("rp_show_prof"))     g_RPConfig.showProfession = j["rp_show_prof"].get<bool>();
            if (j.contains("rp_show_party"))    g_RPConfig.showPartySize  = j["rp_show_party"].get<bool>();
            if (j.contains("rp_app_id")) {
                std::string aid = j["rp_app_id"].get<std::string>();
                strncpy(g_RPAppIdOverride, aid.c_str(), sizeof(g_RPAppIdOverride) - 1);
            }
```

- [ ] **Step 4: Update SaveConfig to write RP settings**

Inside `SaveConfig()`, after the existing `j["hide_on_hover"] = ...` line, add:

```cpp
        j["rp_enabled"]    = g_RPConfig.enabled;
        j["rp_show_name"]  = g_RPConfig.showCharName;
        j["rp_show_map"]   = g_RPConfig.showMap;
        j["rp_show_prof"]  = g_RPConfig.showProfession;
        j["rp_show_party"] = g_RPConfig.showPartySize;
        if (g_RPAppIdOverride[0] != '\0')
            j["rp_app_id"] = std::string(g_RPAppIdOverride);
```

- [ ] **Step 5: Instantiate RichPresence and subscribe to MumbleLink in AddonLoad**

In `AddonLoad`, after `g_Discord = new DiscordClient();`, add:

```cpp
    g_RichPresence = new RichPresence(g_Discord);
    g_MumbleLink = (Mumble::Data*)APIDefs->DataLink_Get(DL_MUMBLE_LINK);
    APIDefs->Events_Subscribe("EV_MUMBLE_IDENTITY_UPDATED", OnMumbleIdentityUpdated);
```

- [ ] **Step 6: Tear down in AddonUnload**

In `AddonUnload`, before the `g_Discord` cleanup block, add:

```cpp
    APIDefs->Events_Unsubscribe("EV_MUMBLE_IDENTITY_UPDATED", OnMumbleIdentityUpdated);
    g_MumbleLink = nullptr;

    if (g_RichPresence) {
        g_RichPresence->Shutdown();
        delete g_RichPresence;
        g_RichPresence = nullptr;
    }
```

- [ ] **Step 7: Call RichPresence::Update from AddonRender**

In `AddonRender`, at the very start of the function (before the ImGui begin), add:

```cpp
    if (g_RichPresence) {
        RPGameState rpState;
        rpState.inGame     = g_MumbleLink && g_MumbleLink->UITick > 0
                             && g_MumbleIdentity.Name[0] != '\0';
        rpState.charName   = g_MumbleIdentity.Name;
        rpState.mapId      = g_MumbleIdentity.MapID;
        rpState.profession = g_MumbleIdentity.Profession;
        g_RichPresence->Update(rpState, g_RPConfig);
    }
```

- [ ] **Step 8: Add Rich Presence section to AddonOptions**

In `AddonOptions()`, after the last `ImGui::ColorEdit4` line, add:

```cpp
    ImGui::Separator();
    ImGui::Text("Rich Presence");
    ImGui::Spacing();

    if (ImGui::Checkbox("Enable Rich Presence", &g_RPConfig.enabled)) SaveConfig();

    if (g_RPConfig.enabled) {
        ImGui::Indent();

        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::InputText("Application ID", g_RPAppIdOverride, sizeof(g_RPAppIdOverride)))
            SaveConfig();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Leave blank to use the built-in application ID.");

        if (ImGui::Checkbox("Show character name", &g_RPConfig.showCharName)) SaveConfig();
        if (ImGui::Checkbox("Show current map",    &g_RPConfig.showMap))      SaveConfig();
        if (ImGui::Checkbox("Show profession",     &g_RPConfig.showProfession)) SaveConfig();

        ImGui::BeginDisabled();
        ImGui::Checkbox("Show party size (not yet available)", &g_RPConfig.showPartySize);
        ImGui::EndDisabled();

        ImGui::Unindent();
    }
```

- [ ] **Step 9: Build**

```bash
cd /home/tony/Dev/discordant/build && make 2>&1 | tail -5
```

Expected: success.

- [ ] **Step 10: Commit**

```bash
git add src/dllmain.cpp
git commit -m "feat: integrate RichPresence into addon lifecycle, config, and options UI"
```

---

## Post-Implementation Testing Checklist

Load the addon in-game and verify in Discord:

- [ ] Rich Presence disabled by default — no activity shown on Discord profile
- [ ] Enable Rich Presence — activity appears (may show under StreamKit app name — expected for this iteration)
- [ ] Character name shown when "Show character name" is checked, hidden when unchecked
- [ ] Map name updates when moving between maps
- [ ] Entering a loading screen shows "Playing Guild Wars 2" instead of stale data
- [ ] Unchecking all data fields shows "Playing Guild Wars 2" fallback
- [ ] Disabling Rich Presence clears the activity immediately
- [ ] Addon reload (Nexus hot-reload) does not crash or leak
- [ ] Application ID field accepts input and persists across sessions
