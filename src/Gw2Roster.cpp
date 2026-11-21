#include "Gw2Roster.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <unordered_map>

#include "nexus/Nexus.h"
#include "rtapi/RTAPI.hpp"

namespace {

// Events raised by Raidcore's "ArcDPS Integration" addon, which relays both
// ArcDPS and Unofficial Extras (RaidcoreGG/GW2-Arcdps-Integration, MIT).
constexpr const char* EV_ACCOUNT_NAME                   = "EV_ACCOUNT_NAME";
constexpr const char* EV_ARCDPS_SELF_JOIN               = "EV_ARCDPS_SELF_JOIN";
constexpr const char* EV_ARCDPS_SELF_LEAVE              = "EV_ARCDPS_SELF_LEAVE";
constexpr const char* EV_ARCDPS_SQUAD_JOIN              = "EV_ARCDPS_SQUAD_JOIN";
constexpr const char* EV_ARCDPS_SQUAD_LEAVE             = "EV_ARCDPS_SQUAD_LEAVE";
constexpr const char* EV_REQUEST_ACCOUNT_NAME           = "EV_REQUEST_ACCOUNT_NAME";
constexpr const char* EV_REPLAY_ARCDPS_SELF_JOIN        = "EV_REPLAY_ARCDPS_SELF_JOIN";
constexpr const char* EV_REPLAY_ARCDPS_SQUAD_JOIN       = "EV_REPLAY_ARCDPS_SQUAD_JOIN";
constexpr const char* EV_UNOFFICIAL_EXTRAS_SQUAD_UPDATE = "EV_UNOFFICIAL_EXTRAS_SQUAD_UPDATE";
constexpr uint32_t    ARCDPS_INTEGRATION_SIG            = (uint32_t)-19392669;

// Payload of the EV_ARCDPS_SQUAD_* / EV_ARCDPS_SELF_* events (Globals.h there).
struct EvAgentUpdate {
    char      account[64];    // with leading ':'
    char      character[64];
    uintptr_t id;
    uintptr_t instanceId;
    uint32_t  added;
    uint32_t  target;
    uint32_t  Self;
    uint32_t  prof;
    uint32_t  elite;
    uint16_t  team;
    uint16_t  subgroup;
};

// Unofficial Extras' UserInfo (Krappa322/arcdps_unofficial_extras_releases,
// Definitions.h, MIT). Read as an array, so the size must match exactly.
struct ExtrasUserInfo {
    const char* AccountName;  // with leading ':'
    int64_t     JoinTime;
    uint8_t     Role;         // EXTRAS_ROLE_NONE: removed from the squad
    uint8_t     Subgroup;
    bool        ReadyStatus;
    uint8_t     GroupType;
    uint32_t    Unused;
};
static_assert(sizeof(ExtrasUserInfo) == 24, "must match Unofficial Extras' UserInfo");
// Roles 0-2 are leader, lieutenant, member. Invited (3) and Applied (4) are
// not in the squad yet, and None (5) means removed.
constexpr uint8_t EXTRAS_ROLE_MEMBER = 2;

struct ExtrasSquadUpdate {
    ExtrasUserInfo* UserInfo;
    uint64_t        UsersCount;
};

const char* SourceName(int src) {
    switch (src) {
    case SRC_RTAPI:  return "RTAPI";
    case SRC_ARCDPS: return "ArcDPS";
    default:         return "Unofficial Extras";
    }
}

struct Entry {
    std::string account;    // display form
    std::string charRtapi;  // RTAPI is preferred: it knows members on other maps
    std::string charArc;
    uint8_t     sources = 0;  // bit per Gw2Source
};

struct State {
    AddonAPI_t* api = nullptr;
    std::unordered_map<std::string, Entry> members;  // keyed by NormalizeAccount
    std::string selfRtapi;
    std::string selfArc;  // from the ArcDPS Integration (ArcDPS or Extras)
    RTAPI::RealTimeData* rtapi = nullptr;
    bool replayPending = false;
    bool active[SRC_COUNT] = {};
    std::vector<std::string> logQueue;
};

// Heap-allocated in Init: no non-trivial globals (DllMain constraint).
State* s = nullptr;

// Guards `s` itself as well as its contents, so a callback arriving on
// another thread during Shutdown either finishes first or sees nullptr.
// SRWLOCK is plain data, so it is safe as a global.
SRWLOCK g_lock = SRWLOCK_INIT;

struct LockGuard {
    LockGuard()  { AcquireSRWLockExclusive(&g_lock); }
    ~LockGuard() { ReleaseSRWLockExclusive(&g_lock); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;
};

// Account names arrive with a leading ':' and in fixed-size buffers.
std::string CleanAccount(const char* name, size_t maxLen) {
    if (!name) return std::string();
    size_t n = strnlen(name, maxLen);
    if (n > 0 && name[0] == ':') { name++; n--; }
    return std::string(name, n);
}

// The helpers below expect g_lock to be held.

void MarkActive(int src) {
    if (s->active[src]) return;
    s->active[src] = true;
    s->logQueue.push_back(std::string("Receiving squad data from ") + SourceName(src));
}

void Join(int src, const std::string& account, const std::string& character) {
    if (account.empty()) return;
    Entry& e = s->members[Gw2Roster::NormalizeAccount(account)];
    e.account = account;
    e.sources |= (uint8_t)(1u << src);
    if (!character.empty())
        (src == SRC_RTAPI ? e.charRtapi : e.charArc) = character;
}

void Leave(int src, const std::string& account) {
    auto it = s->members.find(Gw2Roster::NormalizeAccount(account));
    if (it == s->members.end()) return;
    Entry& e = it->second;
    e.sources &= (uint8_t)~(1u << src);
    if (src == SRC_RTAPI)  e.charRtapi.clear();
    if (src == SRC_ARCDPS) e.charArc.clear();
    if (e.sources == 0) s->members.erase(it);
}

void ClearSource(int src) {
    std::vector<std::string> accounts;
    for (const auto& kv : s->members)
        if (kv.second.sources & (1u << src)) accounts.push_back(kv.second.account);
    for (const auto& a : accounts) Leave(src, a);
}

// The addon providing `src` went away.
void DropSource(int src) {
    ClearSource(src);
    if (s->active[src])
        s->logQueue.push_back(std::string("Stopped receiving squad data from ") + SourceName(src));
    s->active[src] = false;
}

// Event callbacks can come from any thread, and an exception escaping one
// terminates the game, so each one catches everything.
template <typename F>
void Guarded(F&& f) {
    try {
        LockGuard lock;
        if (!s) return;
        f();
    } catch (...) {}
}

void OnRtapiMember(void* args, bool left) {
    auto* m = static_cast<const RTAPI::GroupMember*>(args);
    if (!m) return;
    Guarded([&] {
        MarkActive(SRC_RTAPI);
        std::string account = CleanAccount(m->AccountName, sizeof(m->AccountName));
        if (m->IsSelf) {
            if (!account.empty()) s->selfRtapi = account;
            return;
        }
        if (left) Leave(SRC_RTAPI, account);
        else      Join(SRC_RTAPI, account,
                       std::string(m->CharacterName, strnlen(m->CharacterName, sizeof(m->CharacterName))));
    });
}
void OnRtapiJoined(void* args)  { OnRtapiMember(args, false); }
void OnRtapiUpdated(void* args) { OnRtapiMember(args, false); }
void OnRtapiLeft(void* args)    { OnRtapiMember(args, true); }

void OnArcSquadJoin(void* args) {
    auto* a = static_cast<const EvAgentUpdate*>(args);
    if (!a) return;
    Guarded([&] {
        MarkActive(SRC_ARCDPS);
        Join(SRC_ARCDPS, CleanAccount(a->account, sizeof(a->account)),
             std::string(a->character, strnlen(a->character, sizeof(a->character))));
    });
}

void OnArcSquadLeave(void* args) {
    auto* a = static_cast<const EvAgentUpdate*>(args);
    if (!a) return;
    Guarded([&] { Leave(SRC_ARCDPS, CleanAccount(a->account, sizeof(a->account))); });
}

void OnArcSelfJoin(void* args) {
    auto* a = static_cast<const EvAgentUpdate*>(args);
    if (!a) return;
    Guarded([&] {
        MarkActive(SRC_ARCDPS);
        std::string account = CleanAccount(a->account, sizeof(a->account));
        if (!account.empty()) s->selfArc = account;
    });
}

// We left the squad: the integration forgets every member without telling us.
void OnArcSelfLeave(void*) {
    Guarded([&] { ClearSource(SRC_ARCDPS); });
}

void OnAccountName(void* args) {
    const char* name = static_cast<const char*>(args);
    Guarded([&] {
        std::string account = CleanAccount(name, 140);
        if (!account.empty()) s->selfArc = account;
    });
}

void OnExtrasSquadUpdate(void* args) {
    auto* u = static_cast<const ExtrasSquadUpdate*>(args);
    if (!u || !u->UserInfo) return;
    Guarded([&] {
        MarkActive(SRC_EXTRAS);
        for (uint64_t i = 0; i < u->UsersCount; i++) {
            const ExtrasUserInfo& info = u->UserInfo[i];
            std::string account = CleanAccount(info.AccountName, 140);
            if (info.Role <= EXTRAS_ROLE_MEMBER) Join(SRC_EXTRAS, account, std::string());
            else                                 Leave(SRC_EXTRAS, account);
        }
    });
}

void OnAddonLoaded(void* args) {
    if (!args) return;
    uint32_t sig = *static_cast<const uint32_t*>(args);
    Guarded([&] {
        if (sig == RTAPI_SIG)
            s->rtapi = static_cast<RTAPI::RealTimeData*>(s->api->DataLink_Get(DL_RTAPI));
        // Raising events from inside an event callback is not safe, so the
        // replay request is sent on the next frame (Update).
        if (sig == ARCDPS_INTEGRATION_SIG)
            s->replayPending = true;
    });
}

void OnAddonUnloaded(void* args) {
    if (!args) return;
    uint32_t sig = *static_cast<const uint32_t*>(args);
    Guarded([&] {
        if (sig == RTAPI_SIG) {
            s->rtapi = nullptr;
            s->selfRtapi.clear();
            DropSource(SRC_RTAPI);
        }
        if (sig == ARCDPS_INTEGRATION_SIG) {
            s->selfArc.clear();
            DropSource(SRC_ARCDPS);
            DropSource(SRC_EXTRAS);
        }
    });
}

struct Subscription { const char* event; void (*callback)(void*); };
const Subscription kSubscriptions[] = {
    {EV_RTAPI_GROUP_MEMBER_JOINED,      OnRtapiJoined},
    {EV_RTAPI_GROUP_MEMBER_UPDATED,     OnRtapiUpdated},
    {EV_RTAPI_GROUP_MEMBER_LEFT,        OnRtapiLeft},
    {EV_ARCDPS_SQUAD_JOIN,              OnArcSquadJoin},
    {EV_ARCDPS_SQUAD_LEAVE,             OnArcSquadLeave},
    {EV_ARCDPS_SELF_JOIN,               OnArcSelfJoin},
    {EV_ARCDPS_SELF_LEAVE,              OnArcSelfLeave},
    {EV_ACCOUNT_NAME,                   OnAccountName},
    {EV_UNOFFICIAL_EXTRAS_SQUAD_UPDATE, OnExtrasSquadUpdate},
    {EV_ADDON_LOADED,                   OnAddonLoaded},
    {EV_ADDON_UNLOADED,                 OnAddonUnloaded},
};

// Ask the ArcDPS Integration to resend what it already knows. Nothing
// answers if it is not running.
void RequestReplay(AddonAPI_t* api) {
    api->Events_Raise(EV_REQUEST_ACCOUNT_NAME, nullptr);
    api->Events_Raise(EV_REPLAY_ARCDPS_SELF_JOIN, nullptr);
    api->Events_Raise(EV_REPLAY_ARCDPS_SQUAD_JOIN, nullptr);
}

bool IsWordByte(unsigned char c) { return std::isalnum(c) || c >= 0x80; }

std::string ToLowerAscii(std::string text) {
    for (char& c : text)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return text;
}

}  // namespace

namespace Gw2Roster {

void Init(AddonAPI_t* api) {
    if (s) return;
    s = new State();
    s->api = api;
    // RTAPI sends its group members to addons loaded after it; if it loads
    // later, EV_ADDON_LOADED picks up its DataLink.
    s->rtapi = static_cast<RTAPI::RealTimeData*>(api->DataLink_Get(DL_RTAPI));
    for (const auto& sub : kSubscriptions)
        api->Events_Subscribe(sub.event, sub.callback);
    RequestReplay(api);
}

void Shutdown() {
    if (!s) return;
    for (const auto& sub : kSubscriptions)
        s->api->Events_Unsubscribe(sub.event, sub.callback);
    LockGuard lock;  // waits for any callback still running
    delete s;
    s = nullptr;
}

void Update() {
    bool replay = false;
    AddonAPI_t* api = nullptr;
    {
        LockGuard lock;
        if (!s) return;
        api = s->api;
        replay = s->replayPending;
        s->replayPending = false;
        // RTAPI knows our own account even when we're not in a group
        if (s->rtapi && s->rtapi->GameBuild != 0) {
            std::string account = CleanAccount(s->rtapi->AccountName, sizeof(s->rtapi->AccountName));
            if (!account.empty()) {
                s->selfRtapi = account;
                MarkActive(SRC_RTAPI);
            }
        }
    }
    // Raised outside the lock: the integration may answer on another thread
    if (replay) RequestReplay(api);
}

Gw2RosterSnapshot GetSnapshot() {
    Gw2RosterSnapshot snap;
    LockGuard lock;
    if (!s) return snap;
    snap.selfAccount = s->selfRtapi.empty() ? s->selfArc : s->selfRtapi;
    std::string selfKey = NormalizeAccount(snap.selfAccount);
    for (const auto& kv : s->members) {
        if (kv.first == selfKey) continue;  // Extras lists us among the squad
        const Entry& e = kv.second;
        snap.members.push_back({e.account, e.charRtapi.empty() ? e.charArc : e.charRtapi});
    }
    std::sort(snap.members.begin(), snap.members.end(),
              [](const Gw2Member& a, const Gw2Member& b) { return a.account < b.account; });
    for (int i = 0; i < SRC_COUNT; i++) snap.sourceActive[i] = s->active[i];
    return snap;
}

std::vector<std::string> DrainLogQueue() {
    std::vector<std::string> out;
    LockGuard lock;
    if (!s) return out;
    out.swap(s->logQueue);
    return out;
}

std::string NormalizeAccount(const std::string& account) {
    size_t start = (!account.empty() && account[0] == ':') ? 1 : 0;
    return ToLowerAscii(account.substr(start));
}

const Gw2Member* FindAccountInText(const std::string& text, const std::vector<Gw2Member>& members) {
    if (text.empty()) return nullptr;
    std::string lower = ToLowerAscii(text);
    for (const auto& m : members) {
        std::string key = NormalizeAccount(m.account);
        if (key.empty()) continue;
        for (size_t pos = lower.find(key); pos != std::string::npos; pos = lower.find(key, pos + 1)) {
            size_t end = pos + key.size();
            bool startOk = pos == 0 || !IsWordByte((unsigned char)lower[pos - 1]);
            bool endOk = end == lower.size() || !std::isdigit((unsigned char)lower[end]);
            if (startOk && endOk) return &m;
        }
    }
    return nullptr;
}

std::string ParseAccountFromText(const std::string& text) {
    const size_t n = text.size();
    for (size_t dot = 1; dot + 4 < n; dot++) {
        // ".1234" not followed by another digit, right after a name character
        if (text[dot] != '.' || !IsWordByte((unsigned char)text[dot - 1])) continue;
        bool digits = true;
        for (size_t k = dot + 1; k <= dot + 4; k++)
            if (!std::isdigit((unsigned char)text[k])) { digits = false; break; }
        if (!digits) continue;
        if (dot + 5 < n && std::isdigit((unsigned char)text[dot + 5])) continue;

        // Walk back over the name (account names may contain spaces), stopping
        // at punctuation such as the "]" of a "[TAG]" or a "|" separator.
        size_t start = dot;
        while (start > 0 && (IsWordByte((unsigned char)text[start - 1]) || text[start - 1] == ' '))
            start--;
        while (start < dot && text[start] == ' ') start++;
        if (dot - start < 3) continue;  // GW2 names have at least 3 characters
        return text.substr(start, dot + 5 - start);
    }
    return std::string();
}

}  // namespace Gw2Roster
