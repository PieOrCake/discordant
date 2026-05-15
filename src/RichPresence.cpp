#include "RichPresence.h"
#include "DiscordIPC.h"
#include "MapData.h"
#include <nlohmann/json.hpp>
#include <windows.h>

using json = nlohmann::json;

void RichPresence::QueueLog(const std::string& msg) {
    std::lock_guard<std::mutex> lock(m_logMutex);
    m_logQueue.push_back(msg);
}

std::vector<std::string> RichPresence::DrainLogQueue() {
    std::lock_guard<std::mutex> lock(m_logMutex);
    std::vector<std::string> out;
    out.swap(m_logQueue);
    return out;
}

RichPresence::RichPresence(DiscordIPC* ipc)
    : m_ipc(ipc)
{}

void RichPresence::Update(const RPGameState& state, const RPConfig& cfg) {
    // Detect any change that would alter the displayed presence
    bool changed =
        cfg.enabled            != m_lastEnabled        ||
        state.inGame           != m_lastInGame         ||
        state.charName         != m_lastCharName       ||
        state.mapId            != m_lastMapId          ||
        state.profession       != m_lastProfession     ||
        state.specialization   != m_lastSpecialization ||
        cfg.showCharName       != m_lastShowName       ||
        cfg.showMap            != m_lastShowMap        ||
        cfg.showProfession     != m_lastShowProf;

    if (!changed) return;

    m_lastEnabled        = cfg.enabled;
    m_lastInGame         = state.inGame;
    m_lastCharName       = state.charName;
    m_lastMapId          = state.mapId;
    m_lastProfession     = state.profession;
    m_lastSpecialization = state.specialization;
    m_lastShowName   = cfg.showCharName;
    m_lastShowMap    = cfg.showMap;
    m_lastShowProf   = cfg.showProfession;

    if (!cfg.enabled) {
        QueueLog("RP: disabled — clearing activity");
        m_ipc->ClearActivity();
        return;
    }

    std::string inGameStr = state.inGame ? "true" : "false";
    QueueLog("RP: sending activity (inGame=" + inGameStr +
             ", char='" + state.charName +
             "', mapId=" + std::to_string(state.mapId) +
             ", prof=" + std::to_string(state.profession) + ")");
    m_ipc->SetActivity(BuildActivityJson(state, cfg));
}

void RichPresence::Shutdown() {
    if (m_lastEnabled)
        m_ipc->ClearActivity();
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
            const char* specKey  = SpecKey(state.specialization);
            const char* specName = SpecName(state.specialization);
            if (specKey && specName) {
                activity["assets"]["large_image"] = specKey;
                activity["assets"]["large_text"]  = specName;
            } else {
                activity["assets"]["large_image"] = ProfessionKey(state.profession);
                activity["assets"]["large_text"]  = ProfessionName(state.profession);
            }
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

const char* RichPresence::SpecName(unsigned id) {
    switch (id) {
    // Guardian
    case 27: return "Dragonhunter";
    case 62: return "Firebrand";
    case 65: return "Willbender";
    case 81: return "Luminary";
    // Warrior
    case 18: return "Berserker";
    case 61: return "Spellbreaker";
    case 68: return "Bladesworn";
    case 74: return "Paragon";
    // Engineer
    case 43: return "Scrapper";
    case 57: return "Holosmith";
    case 70: return "Mechanist";
    case 75: return "Amalgam";
    // Ranger
    case  5: return "Druid";
    case 55: return "Soulbeast";
    case 72: return "Untamed";
    case 78: return "Galeshot";
    // Thief
    case  7: return "Daredevil";
    case 58: return "Deadeye";
    case 71: return "Specter";
    case 77: return "Antiquary";
    // Elementalist
    case 48: return "Tempest";
    case 56: return "Weaver";
    case 67: return "Catalyst";
    case 80: return "Evoker";
    // Mesmer
    case 40: return "Chronomancer";
    case 59: return "Mirage";
    case 66: return "Virtuoso";
    case 73: return "Troubadour";
    // Necromancer
    case 34: return "Reaper";
    case 60: return "Scourge";
    case 64: return "Harbinger";
    case 76: return "Ritualist";
    // Revenant
    case 52: return "Herald";
    case 63: return "Renegade";
    case 69: return "Vindicator";
    case 79: return "Conduit";
    default: return nullptr;
    }
}

const char* RichPresence::SpecKey(unsigned id) {
    switch (id) {
    case 27: return "dragonhunter";
    case 62: return "firebrand";
    case 65: return "willbender";
    case 81: return "luminary";
    case 18: return "berserker";
    case 61: return "spellbreaker";
    case 68: return "bladesworn";
    case 74: return "paragon";
    case 43: return "scrapper";
    case 57: return "holosmith";
    case 70: return "mechanist";
    case 75: return "amalgam";
    case  5: return "druid";
    case 55: return "soulbeast";
    case 72: return "untamed";
    case 78: return "galeshot";
    case  7: return "daredevil";
    case 58: return "deadeye";
    case 71: return "specter";
    case 77: return "antiquary";
    case 48: return "tempest";
    case 56: return "weaver";
    case 67: return "catalyst";
    case 80: return "evoker";
    case 40: return "chronomancer";
    case 59: return "mirage";
    case 66: return "virtuoso";
    case 73: return "troubadour";
    case 34: return "reaper";
    case 60: return "scourge";
    case 64: return "harbinger";
    case 76: return "ritualist";
    case 52: return "herald";
    case 63: return "renegade";
    case 69: return "vindicator";
    case 79: return "conduit";
    default: return nullptr;
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
