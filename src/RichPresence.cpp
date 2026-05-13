#include "RichPresence.h"
#include "DiscordClient.h"
#include "MapData.h"
#include <nlohmann/json.hpp>
#include <windows.h>

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
        cfg.showProfession != m_lastShowProf   ||
        cfg.showPartySize  != m_lastShowParty;

    if (!changed) return;

    m_lastEnabled    = cfg.enabled;
    m_lastInGame     = state.inGame;
    m_lastCharName   = state.charName;
    m_lastMapId      = state.mapId;
    m_lastProfession = state.profession;
    m_lastShowName   = cfg.showCharName;
    m_lastShowMap    = cfg.showMap;
    m_lastShowProf   = cfg.showProfession;
    m_lastShowParty  = cfg.showPartySize;

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
        // Not in game, or no data yet (char select, loading screen)
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
    j["nonce"]         = "rp_" + std::to_string(++m_nonce);
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
