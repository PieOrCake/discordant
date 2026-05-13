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
    bool        m_lastShowParty  = false;
    mutable unsigned m_nonce = 0;
};
