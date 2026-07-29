#ifndef DISCORDANT_DISCORD_CLIENT_H
#define DISCORDANT_DISCORD_CLIENT_H

#include "WebSocket.h"
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <cstdint>

// Represents a user in the voice channel
struct VoiceUser {
    std::string id;
    std::string username;
    std::string avatar;
    std::string nick;       // server nickname
    bool mute = false;      // server muted or self-muted or suppressed
    bool deaf = false;      // server deafened or self-deafened
    bool speaking = false;
};

// Snapshot of all voice state needed for rendering (grabbed in one lock)
struct VoiceSnapshot {
    std::vector<VoiceUser> users;
    std::string channelName;
    std::string channelIconId;
    std::string channelIconName;
};

// Connection state for the Discord RPC client
enum class DiscordState {
    Disconnected,
    Connecting,
    WaitingForReady,
    Authorizing,
    ExchangingToken,
    Authenticating,
    Connected
};

class DiscordClient {
public:
    DiscordClient();
    ~DiscordClient();

    // Initiate connection. Starts background networking thread.
    void Connect();

    // Disconnect and reset state.
    void Disconnect();

    // Set a cached access token (loaded from config).
    void SetAccessToken(const std::string& token) { m_accessToken = token; }

    // Get current access token (for saving to config).
    std::string GetAccessToken() const { return m_accessToken; }

    // Get current connection state.
    DiscordState GetState() const { return m_state.load(); }

    // Get a snapshot of users currently in the voice channel.
    // Thread-safe: locks internally.
    std::vector<VoiceUser> GetVoiceUsers();

    // Get the name of the current voice channel (empty if none).
    std::string GetChannelName();

    // Get channel icon info (custom emoji ID or unicode emoji name).
    std::string GetChannelIconId();
    std::string GetChannelIconName();

    // Get all voice state in one lock (avoids 4 separate mutex acquisitions per frame).
    VoiceSnapshot GetVoiceSnapshot();

    // Get connection status text for display.
    std::string GetStatusText() const;

    // Drain pending log messages (call from render thread to forward to Nexus log).
    std::vector<std::string> DrainLogQueue();

    // Whether the local user is muted/deafened
    bool IsSelfMuted() const { return m_selfMuted; }
    bool IsSelfDeafened() const { return m_selfDeafened; }

private:
    // StreamKit's approved client ID
    static constexpr const char* STREAMKIT_CLIENT_ID = "207646673902501888";

    // Discord RPC port range
    static constexpr int RPC_PORT_MIN = 6463;
    static constexpr int RPC_PORT_MAX = 6472;

    // Process a received JSON message.
    void HandleMessage(const std::string& message);
    void HandleMessageImpl(const std::string& message);

    // Auth flow helpers
    void StartAuthorize();
    void ExchangeToken(const std::string& code);
    void Authenticate();

    // Subscription helpers
    void SubscribeVoiceChannel(const std::string& channelId);
    void UnsubscribeVoiceChannel(const std::string& channelId);
    void SubscribeGlobalEvents();

    // Request current voice channel
    void RequestSelectedVoiceChannel();

    // Update user data, preserving fields not in the update
    void UpdateUser(const std::string& id, const VoiceUser& partial);
    void RemoveUser(const std::string& id);

    // Background thread for all networking
    void NetworkThreadMain();
    void NetworkLoop();
    std::thread m_networkThread;
    std::atomic<bool> m_running{false};

    WebSocket m_ws;
    std::atomic<DiscordState> m_state{DiscordState::Disconnected};
    std::string m_accessToken;
    std::string m_userId;        // our own user ID
    std::string m_currentVoice;  // current voice channel ID
    std::string m_channelName;
    std::string m_channelIconId;    // custom emoji ID (empty for unicode)
    std::string m_channelIconName;  // emoji name or unicode character
    bool m_selfMuted = false;
    bool m_selfDeafened = false;

    // Reconnect timing
    uint64_t m_reconnectAt = 0;  // GetTickCount64() value
    int m_currentPort;           // port being tried

    // Diagnostics: when we entered WaitingForReady, so a Discord client that
    // accepts the socket but never sends READY doesn't just sit there silently.
    uint64_t m_waitingForReadyAt = 0;
    bool m_readyTimeoutLogged = false;

    // Voice user list
    std::mutex m_userMutex;
    std::vector<VoiceUser> m_users;  // maintained in channel join order

    // Token exchange runs on a background thread
    std::atomic<bool> m_tokenExchangePending{false};
    std::atomic<bool> m_tokenExchangeFailed{false};
    std::mutex m_tokenMutex;
    std::string m_pendingAccessToken;  // protected by m_tokenMutex
    std::thread m_tokenThread;

    // Thread-safe log queue (written by network thread, drained by render thread)
    void QueueLog(const std::string& msg);
    std::vector<std::string> m_logQueue;
    std::mutex m_logMutex;
};

#endif // DISCORDANT_DISCORD_CLIENT_H
