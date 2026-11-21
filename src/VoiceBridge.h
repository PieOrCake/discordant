#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct AddonAPI_t;

// Publishes the voice channel to other addons (Pie UI) through the
// DISCORDANT_VOICE DataLink. See include/bridge/PieUIBridge.h.
namespace VoiceBridge {
    struct User {
        std::string account;      // matched GW2 account, or empty
        std::string discordName;
        std::string discordId;
        uint32_t    flags = 0;    // Bridge::DISCORDANT_USER_*
    };

    // Share the block. Call from AddonLoad.
    void Init(AddonAPI_t* api);
    // Write the current state; bumps Sequence only when something changed.
    // Call once per frame from the render thread.
    void Publish(bool active, const std::string& channelName, const std::vector<User>& users);
    // Mark the block inactive. Call from AddonUnload.
    void Shutdown();
}
