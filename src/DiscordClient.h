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
    std::string globalName; // Discord display name
    bool isSelf = false;    // the signed-in user
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

    // Start the background networking thread. If it is already running and
    // waiting to retry, retry immediately instead.
    void Connect();

    // Stop the networking thread and reset state. Blocks until threads exit.
    void Disconnect();

    // Access token (loaded from / saved to config). Thread-safe.
    void SetAccessToken(const std::string& token);
    std::string GetAccessToken() const;

    // Bumped every time the network thread changes the token, so the render
    // thread can cheaply tell when it needs saving.
    uint32_t GetTokenGeneration() const { return m_tokenGeneration.load(); }

    // Get current connection state.
    DiscordState GetState() const { return m_state.load(); }

    // Get all voice state in one lock.
    VoiceSnapshot GetVoiceSnapshot();

    // Get connection status text for display.
    std::string GetStatusText() const;

    // Drain pending log messages (call from render thread to forward to Nexus log).
    std::vector<std::string> DrainLogQueue();

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
    void TokenThreadMain(std::string code);
    void Authenticate();
    void StoreToken(const std::string& token);

    // Subscription helpers
    void SubscribeVoiceChannel(const std::string& channelId);
    void UnsubscribeVoiceChannel(const std::string& channelId);
    void SubscribeGlobalEvents();

    // Request current voice channel
    void RequestSelectedVoiceChannel();

    // Update user data, preserving fields not in the update
    void UpdateUser(const std::string& id, const VoiceUser& partial);
    void RemoveUser(const std::string& id);

    // Drop the connection and all voice state, retry after delayMs.
    void ResetConnection(uint64_t delayMs);

    // Background thread for all networking
    void NetworkThreadMain();
    void NetworkLoop();
    std::thread m_networkThread;
    std::atomic<bool> m_running{false};

    WebSocket m_ws;
    std::atomic<DiscordState> m_state{DiscordState::Disconnected};
    std::string m_userId;        // our own user ID (network thread only)
    std::string m_currentVoice;  // current voice channel ID (network thread only)

    // Guarded by m_tokenMutex
    mutable std::mutex m_tokenMutex;
    std::string m_accessToken;
    std::string m_pendingAccessToken;
    std::atomic<uint32_t> m_tokenGeneration{0};

    // Reconnect timing
    std::atomic<uint64_t> m_reconnectAt{0};  // GetTickCount64() value
    int m_currentPort;                       // port being tried

    // Diagnostics: when we entered WaitingForReady, so a Discord client that
    // accepts the socket but never sends READY doesn't just sit there silently.
    uint64_t m_waitingForReadyAt = 0;
    bool m_readyTimeoutLogged = false;

    // Voice state, guarded by m_userMutex
    std::mutex m_userMutex;
    std::vector<VoiceUser> m_users;  // maintained in channel join order
    std::string m_channelName;
    std::string m_channelIconId;    // custom emoji ID (empty for unicode)
    std::string m_channelIconName;  // emoji name or unicode character

    // Token exchange runs on a background thread
    std::atomic<bool> m_tokenExchangePending{false};
    std::atomic<bool> m_tokenExchangeFailed{false};
    std::thread m_tokenThread;

    // Thread-safe log queue (written by background threads, drained by render thread)
    void QueueLog(const std::string& msg);
    std::vector<std::string> m_logQueue;
    std::mutex m_logMutex;
};

#endif // DISCORDANT_DISCORD_CLIENT_H
