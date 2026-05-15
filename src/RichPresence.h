#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <cstdint>

class DiscordIPC;

// Per-frame game state passed in from dllmain
struct RPGameState {
    bool        inGame;           // false = char select or loading screen
    std::string charName;
    uint32_t    mapId;
    unsigned    profession;       // 0=unknown, 1=Guardian ... 9=Revenant
    unsigned    specialization;   // elite spec ID, or 0 if core
};

// User configuration for what to include in the presence
struct RPConfig {
    bool enabled        = false;
    bool showCharName   = true;
    bool showMap        = true;
    bool showProfession = true;
};

class RichPresence {
public:
    explicit RichPresence(DiscordIPC* ipc);

    // Call once per frame from AddonRender. Sends activity only when state changes.
    void Update(const RPGameState& state, const RPConfig& cfg);

    // Call when the addon unloads to clear the activity.
    void Shutdown();

    // Drain log messages — call from render thread every N frames.
    std::vector<std::string> DrainLogQueue();

private:
    void QueueLog(const std::string& msg);
    std::string BuildActivityJson(const RPGameState& state, const RPConfig& cfg) const;
    static const char* ProfessionName(unsigned prof);
    static const char* ProfessionKey(unsigned prof);    // Discord asset key
    static const char* SpecName(unsigned specId);       // nullptr if not an elite spec
    static const char* SpecKey(unsigned specId);        // Discord asset key for elite spec

    DiscordIPC* m_ipc;

    // Cached state to detect changes
    bool        m_lastEnabled    = false;
    bool        m_lastInGame     = false;
    std::string m_lastCharName;
    uint32_t    m_lastMapId      = 0;
    unsigned    m_lastProfession     = 0;
    unsigned    m_lastSpecialization = 0;
    bool        m_lastShowName   = false;
    bool        m_lastShowMap    = false;
    bool        m_lastShowProf   = false;
    mutable unsigned m_nonce = 0;

    std::mutex               m_logMutex;
    std::vector<std::string> m_logQueue;
};
