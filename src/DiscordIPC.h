#pragma once
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <windows.h>

class DiscordIPC {
public:
    DiscordIPC();
    ~DiscordIPC();

    // Start background thread with the given Discord Application ID.
    void Start(const std::string& appId);

    // Stop background thread and close pipe.
    void Stop();

    // Change the App ID and reconnect. Safe to call from any thread.
    void SetAppId(const std::string& appId);

    // Queue a Rich Presence activity update. Thread-safe.
    // activityJson must be a complete Discord RPC envelope:
    //   {"cmd":"SET_ACTIVITY","nonce":"...","args":{"pid":...,"activity":{...}}}
    void SetActivity(const std::string& activityJson);

    // Queue a presence clear. Thread-safe.
    void ClearActivity();

private:
    void ThreadMain();
    bool Connect();
    void Disconnect();
    bool SendFrame(uint32_t opcode, const std::string& payload);
    bool ReadFrame(uint32_t& opcode, std::string& payload);

    HANDLE              m_pipe = INVALID_HANDLE_VALUE;
    std::string         m_appId;
    std::mutex          m_appIdMutex;
    std::atomic<bool>   m_reconnect{false};

    std::thread         m_thread;
    std::atomic<bool>   m_running{false};

    std::mutex          m_activityMutex;
    std::string         m_pendingActivity;
    std::atomic<bool>   m_activityPending{false};

    uint64_t            m_reconnectAt = 0;
};
