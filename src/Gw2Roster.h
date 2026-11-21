#pragma once
#include <string>
#include <vector>

struct AddonAPI_t;

// GW2 squad/party data from whichever optional addons are running:
//   RTAPI          - account + character names (party and squad, any map)
//   ArcDPS         - account + character names, via Raidcore's ArcDPS Integration
//   Unofficial Extras - account names only, via the same integration
//   Pie UI         - account + character names, from its PIEUI_GROUP DataLink
// None of them is required; each is used if it delivers data.
enum Gw2Source { SRC_RTAPI = 0, SRC_ARCDPS, SRC_EXTRAS, SRC_PIEUI, SRC_COUNT };

struct Gw2Member {
    std::string account;    // "Name.1234", without the leading ':'
    std::string character;  // empty if no source knows it
};

struct Gw2RosterSnapshot {
    std::string selfAccount;          // empty if no source knows it
    std::vector<Gw2Member> members;   // squad/party members other than self
    bool sourceActive[SRC_COUNT] = {};  // delivered data since load
};

namespace Gw2Roster {
    // Subscribe to the addons' events and ask those already running to
    // replay their state. Call from AddonLoad.
    void Init(AddonAPI_t* api);
    // Unsubscribe and free everything. Call from AddonUnload.
    void Shutdown();
    // Call once per frame from the render thread.
    void Update();

    // Thread-safe copy of the current roster.
    Gw2RosterSnapshot GetSnapshot();

    // Drain log messages (call from the render thread).
    std::vector<std::string> DrainLogQueue();

    // Lower-case ASCII, leading ':' removed: the form used to compare accounts.
    std::string NormalizeAccount(const std::string& account);

    // The member whose account appears in `text` (e.g. a Discord nickname like
    // "[TAG] Bob.1234"), or nullptr. The ".1234" must be present.
    const Gw2Member* FindAccountInText(const std::string& text, const std::vector<Gw2Member>& members);

    // An account name written in `text` ("Name.1234"), or empty.
    std::string ParseAccountFromText(const std::string& text);
}
