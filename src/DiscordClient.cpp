#include "DiscordClient.h"
#include <nlohmann/json.hpp>
#include <windows.h>
#include <wininet.h>
#include <thread>
#include <algorithm>

using json = nlohmann::json;

DiscordClient::DiscordClient()
    : m_currentPort(RPC_PORT_MIN)
    , m_reconnectAt(0)
{
}

DiscordClient::~DiscordClient() {
    Disconnect();
}

void DiscordClient::Connect() {
    if (m_running.load()) return;
    if (m_state.load() != DiscordState::Disconnected) return;
    m_running.store(true);
    m_state.store(DiscordState::Connecting);
    m_currentPort = RPC_PORT_MIN;
    m_reconnectAt = 0;
    m_networkThread = std::thread(&DiscordClient::NetworkThreadMain, this);
}

void DiscordClient::Disconnect() {
    m_running.store(false);
    if (m_networkThread.joinable()) {
        m_networkThread.join();
    }
    if (!m_currentVoice.empty()) {
        // Don't send unsubscribe after thread stopped — ws is closed
    }
    m_ws.Close();
    m_state.store(DiscordState::Disconnected);
    m_currentVoice.clear();
    m_channelName.clear();
    m_userId.clear();
    m_selfMuted = false;
    m_selfDeafened = false;
    {
        std::lock_guard<std::mutex> lock(m_userMutex);
        m_users.clear();
        m_inRoom.clear();
    }
}

std::vector<VoiceUser> DiscordClient::GetVoiceUsers() {
    std::lock_guard<std::mutex> lock(m_userMutex);
    // Return users in room order
    std::vector<VoiceUser> result;
    for (const auto& uid : m_inRoom) {
        for (const auto& u : m_users) {
            if (u.id == uid) {
                result.push_back(u);
                break;
            }
        }
    }
    return result;
}

std::string DiscordClient::GetChannelName() {
    std::lock_guard<std::mutex> lock(m_userMutex);
    return m_channelName;
}

std::string DiscordClient::GetChannelIconId() {
    std::lock_guard<std::mutex> lock(m_userMutex);
    return m_channelIconId;
}

std::string DiscordClient::GetChannelIconName() {
    std::lock_guard<std::mutex> lock(m_userMutex);
    return m_channelIconName;
}

void DiscordClient::QueueLog(const std::string& msg) {
    std::lock_guard<std::mutex> lock(m_logMutex);
    m_logQueue.push_back(msg);
}

std::vector<std::string> DiscordClient::DrainLogQueue() {
    std::lock_guard<std::mutex> lock(m_logMutex);
    std::vector<std::string> out;
    out.swap(m_logQueue);
    return out;
}

std::string DiscordClient::GetStatusText() const {
    switch (m_state.load()) {
    case DiscordState::Disconnected:    return "Disconnected";
    case DiscordState::Connecting:      return "Connecting...";
    case DiscordState::WaitingForReady: return "Waiting for Discord...";
    case DiscordState::Authorizing:     return "Authorizing (check Discord)...";
    case DiscordState::ExchangingToken: return "Exchanging token...";
    case DiscordState::Authenticating:  return "Authenticating...";
    case DiscordState::Connected:       return "Connected";
    default: return "Unknown";
    }
}

void DiscordClient::NetworkThreadMain() {
    QueueLog("Network thread started");
    while (m_running.load()) {
        // Handle token exchange completion
        if (m_tokenExchangePending) {
            if (m_tokenExchangeFailed) {
                m_tokenExchangePending = false;
                m_tokenExchangeFailed = false;
                m_state.store(DiscordState::Disconnected);
                m_reconnectAt = GetTickCount64() + 30000;
                Sleep(100);
                continue;
            }
            if (!m_pendingAccessToken.empty()) {
                m_accessToken = m_pendingAccessToken;
                m_pendingAccessToken.clear();
                m_tokenExchangePending = false;
                Authenticate();
                Sleep(16);
                continue;
            }
            // Still waiting
            Sleep(100);
            continue;
        }

        // Connection state machine
        DiscordState curState = m_state.load();
        switch (curState) {
        case DiscordState::Disconnected: {
            uint64_t now = GetTickCount64();
            if (m_reconnectAt > 0 && now < m_reconnectAt) {
                Sleep(500);
                continue;
            }
            m_state.store(DiscordState::Connecting);
            m_currentPort = RPC_PORT_MIN;
        } // fallthrough
        case DiscordState::Connecting: {
            // Try each port (blocking connect is fine on background thread)
            while (m_currentPort <= RPC_PORT_MAX && m_running.load()) {
                std::string path = "/?v=1&client_id=";
                path += STREAMKIT_CLIENT_ID;
                if (m_ws.Connect("127.0.0.1", m_currentPort, path, "http://localhost:3000")) {
                    QueueLog("WebSocket connected on port " + std::to_string(m_currentPort));
                    m_state.store(DiscordState::WaitingForReady);
                    break;
                }
                m_currentPort++;
            }
            if (m_state.load() != DiscordState::WaitingForReady) {
                // All ports failed — schedule reconnect
                m_state.store(DiscordState::Disconnected);
                m_reconnectAt = GetTickCount64() + 10000;
            }
            Sleep(16);
            continue;
        }
        case DiscordState::WaitingForReady:
        case DiscordState::Authorizing:
        case DiscordState::Authenticating:
        case DiscordState::Connected:
            break;
        default:
            Sleep(100);
            continue;
        }

        // If WebSocket disconnected unexpectedly, schedule reconnect
        if (m_ws.GetState() != WebSocket::State::Connected) {
            m_state.store(DiscordState::Disconnected);
            m_reconnectAt = GetTickCount64() + 10000;
            {
                std::lock_guard<std::mutex> lock(m_userMutex);
                m_users.clear();
                m_inRoom.clear();
            }
            m_currentVoice.clear();
            m_channelName.clear();
            Sleep(100);
            continue;
        }

        // Poll for messages
        auto messages = m_ws.Poll();
        for (const auto& msg : messages) {
            HandleMessage(msg);
        }

        // Sleep briefly to avoid busy-spinning
        Sleep(16);
    }
}

void DiscordClient::SendCommand(const std::string& cmd, const std::string& args,
                                const std::string& evt, const std::string& nonce) {
    json j;
    j["cmd"] = cmd;
    j["args"] = json::parse(args);
    if (!evt.empty()) j["evt"] = evt;
    j["nonce"] = nonce.empty() ? "discordant" : nonce;
    m_ws.SendText(j.dump());
}

void DiscordClient::HandleMessage(const std::string& message) {
    // Log raw message for diagnostics (truncated)
    QueueLog("RPC recv: " + message.substr(0, 200));

    json j;
    try {
        j = json::parse(message);
    } catch (...) {
        QueueLog("HandleMessage: JSON parse failed");
        return;
    }

    std::string cmd = j.contains("cmd") && j["cmd"].is_string() ? j["cmd"].get<std::string>() : "";
    std::string evt = j.contains("evt") && j["evt"].is_string() ? j["evt"].get<std::string>() : "";

    // --- DISPATCH events ---
    if (cmd == "DISPATCH") {
        if (evt == "READY") {
            // Connection established, start auth
            if (!m_accessToken.empty()) {
                Authenticate();
            } else {
                StartAuthorize();
            }
            return;
        }

        if (evt == "VOICE_STATE_UPDATE") {
            std::lock_guard<std::mutex> lock(m_userMutex);
            auto& d = j["data"];
            VoiceUser u;
            u.id = d["user"].value("id", "");
            u.username = d["user"].value("username", "");
            u.avatar = d["user"].contains("avatar") && d["user"]["avatar"].is_string() ? d["user"]["avatar"].get<std::string>() : "";
            u.nick = d.contains("nick") && d["nick"].is_string() ? d["nick"].get<std::string>() : "";
            auto& vs = d["voice_state"];
            u.mute = vs.value("mute", false) || vs.value("self_mute", false) || vs.value("suppress", false);
            u.deaf = vs.value("deaf", false) || vs.value("self_deaf", false);
            UpdateUser(u.id, u);
            // Ensure user is in room
            if (std::find(m_inRoom.begin(), m_inRoom.end(), u.id) == m_inRoom.end()) {
                m_inRoom.push_back(u.id);
            }
            return;
        }

        if (evt == "VOICE_STATE_CREATE") {
            std::lock_guard<std::mutex> lock(m_userMutex);
            auto& d = j["data"];
            VoiceUser u;
            u.id = d["user"].value("id", "");
            u.username = d["user"].value("username", "");
            u.avatar = d["user"].contains("avatar") && d["user"]["avatar"].is_string() ? d["user"]["avatar"].get<std::string>() : "";
            u.nick = d.contains("nick") && d["nick"].is_string() ? d["nick"].get<std::string>() : "";
            if (d.contains("voice_state")) {
                auto& vs = d["voice_state"];
                u.mute = vs.value("mute", false) || vs.value("self_mute", false) || vs.value("suppress", false);
                u.deaf = vs.value("deaf", false) || vs.value("self_deaf", false);
            }
            UpdateUser(u.id, u);
            if (std::find(m_inRoom.begin(), m_inRoom.end(), u.id) == m_inRoom.end()) {
                m_inRoom.push_back(u.id);
            }
            // If it's us, find our channel
            if (u.id == m_userId) {
                RequestSelectedVoiceChannel();
            }
            return;
        }

        if (evt == "VOICE_STATE_DELETE") {
            std::lock_guard<std::mutex> lock(m_userMutex);
            std::string uid = j["data"]["user"].value("id", "");
            RemoveUser(uid);
            m_inRoom.erase(std::remove(m_inRoom.begin(), m_inRoom.end(), uid), m_inRoom.end());
            if (uid == m_userId) {
                m_inRoom.clear();
                m_users.clear();
                RequestSelectedVoiceChannel();
            }
            return;
        }

        if (evt == "SPEAKING_START") {
            std::lock_guard<std::mutex> lock(m_userMutex);
            std::string uid = j["data"].value("user_id", "");
            for (auto& u : m_users) {
                if (u.id == uid) { u.speaking = true; break; }
            }
            // Ensure user is tracked as in room
            if (std::find(m_inRoom.begin(), m_inRoom.end(), uid) == m_inRoom.end()) {
                m_inRoom.push_back(uid);
            }
            return;
        }

        if (evt == "SPEAKING_STOP") {
            std::lock_guard<std::mutex> lock(m_userMutex);
            std::string uid = j["data"].value("user_id", "");
            for (auto& u : m_users) {
                if (u.id == uid) { u.speaking = false; break; }
            }
            return;
        }

        if (evt == "VOICE_CHANNEL_SELECT") {
            auto& d = j["data"];
            std::string chId = d.contains("channel_id") && d["channel_id"].is_string() ? d["channel_id"].get<std::string>() : "";
            std::string guildId = d.contains("guild_id") && d["guild_id"].is_string() ? d["guild_id"].get<std::string>() : "";
            if (!chId.empty()) {
                if (chId != m_currentVoice) {
                    if (!m_currentVoice.empty()) {
                        UnsubscribeVoiceChannel(m_currentVoice);
                    }
                    {
                        std::lock_guard<std::mutex> lock(m_userMutex);
                        m_users.clear();
                        m_inRoom.clear();
                    }
                    SubscribeVoiceChannel(chId);
                    m_currentVoice = chId;
                    RequestSelectedVoiceChannel();
                }
            } else {
                // Left voice
                if (!m_currentVoice.empty()) {
                    UnsubscribeVoiceChannel(m_currentVoice);
                }
                m_currentVoice.clear();
                {
                    std::lock_guard<std::mutex> lock(m_userMutex);
                    m_users.clear();
                    m_inRoom.clear();
                    m_channelName.clear();
                    m_channelIconId.clear();
                    m_channelIconName.clear();
                }
            }
            return;
        }

        if (evt == "VOICE_SETTINGS_UPDATE") {
            auto& d = j["data"];
            m_selfMuted = d.value("mute", false);
            m_selfDeafened = d.value("deaf", false);
            return;
        }

        // Ignore other dispatch events
        return;
    }

    // --- Command responses ---
    if (cmd == "AUTHORIZE") {
        if (j.contains("data") && j["data"].contains("code")) {
            std::string code = j["data"]["code"];
            ExchangeToken(code);
        } else {
            // Authorization denied
            m_state.store(DiscordState::Disconnected);
            m_reconnectAt = GetTickCount64() + 60000;
        }
        return;
    }

    if (cmd == "AUTHENTICATE") {
        if (evt == "ERROR") {
            // Token expired or invalid, re-authorize
            m_accessToken.clear();
            StartAuthorize();
            return;
        }
        if (j.contains("data") && j["data"].contains("user")) {
            m_userId = j["data"]["user"].value("id", "");
            m_state.store(DiscordState::Connected);
            SubscribeGlobalEvents();
            RequestSelectedVoiceChannel();
        }
        return;
    }

    if (cmd == "GET_SELECTED_VOICE_CHANNEL") {
        if (!j.contains("data") || j["data"].is_null()) {
            // Not in a voice channel
            if (!m_currentVoice.empty()) {
                UnsubscribeVoiceChannel(m_currentVoice);
            }
            m_currentVoice.clear();
            {
                std::lock_guard<std::mutex> lock(m_userMutex);
                m_users.clear();
                m_inRoom.clear();
                m_channelName.clear();
            }
            return;
        }

        auto& d = j["data"];
        std::string chId = d.contains("id") && d["id"].is_string() ? d["id"].get<std::string>() : "";
        std::string guildId = d.contains("guild_id") && d["guild_id"].is_string() ? d["guild_id"].get<std::string>() : "";
        std::string chName = d.contains("name") && d["name"].is_string() ? d["name"].get<std::string>() : "";

        if (chId != m_currentVoice) {
            if (!m_currentVoice.empty()) {
                UnsubscribeVoiceChannel(m_currentVoice);
            }
            SubscribeVoiceChannel(chId);
            m_currentVoice = chId;
        }

        {
            std::lock_guard<std::mutex> lock(m_userMutex);
            m_channelName = chName;
            // Parse channel icon_emoji
            m_channelIconId.clear();
            m_channelIconName.clear();
            if (d.contains("icon_emoji") && d["icon_emoji"].is_object()) {
                auto& ie = d["icon_emoji"];
                m_channelIconId = ie.contains("id") && ie["id"].is_string() ? ie["id"].get<std::string>() : "";
                m_channelIconName = ie.contains("name") && ie["name"].is_string() ? ie["name"].get<std::string>() : "";
            }
            m_users.clear();
            m_inRoom.clear();

            if (d.contains("voice_states") && d["voice_states"].is_array()) {
                for (auto& vs : d["voice_states"]) {
                    VoiceUser u;
                    u.id = vs["user"].value("id", "");
                    u.username = vs["user"].value("username", "");
                    u.avatar = vs["user"].contains("avatar") && vs["user"]["avatar"].is_string() ? vs["user"]["avatar"].get<std::string>() : "";
                    u.nick = vs.contains("nick") && vs["nick"].is_string() ? vs["nick"].get<std::string>() : "";
                    if (vs.contains("voice_state")) {
                        auto& vstate = vs["voice_state"];
                        u.mute = vstate.value("mute", false) || vstate.value("self_mute", false) || vstate.value("suppress", false);
                        u.deaf = vstate.value("deaf", false) || vstate.value("self_deaf", false);
                    }
                    m_users.push_back(u);
                    m_inRoom.push_back(u.id);
                }
            }
        }
        return;
    }

    // SUBSCRIBE/UNSUBSCRIBE confirmations - ignore
    if (cmd == "SUBSCRIBE" || cmd == "UNSUBSCRIBE") return;
}

void DiscordClient::StartAuthorize() {
    QueueLog("Sending AUTHORIZE with prompt=consent");
    m_state.store(DiscordState::Authorizing);
    json args;
    args["client_id"] = STREAMKIT_CLIENT_ID;
    args["scopes"] = json::array({"rpc", "messages.read", "rpc.notifications.read"});
    args["prompt"] = "consent";

    json j;
    j["cmd"] = "AUTHORIZE";
    j["args"] = args;
    j["nonce"] = "discordant_auth";
    m_ws.SendText(j.dump());
}

void DiscordClient::ExchangeToken(const std::string& code) {
    QueueLog("ExchangeToken: starting with code " + code.substr(0, 8) + "...");
    m_state.store(DiscordState::ExchangingToken);
    m_tokenExchangePending = true;
    m_tokenExchangeFailed = false;
    m_pendingAccessToken.clear();

    // Background thread for HTTP POST to StreamKit token endpoint
    std::string codeCopy = code;
    std::thread([this, codeCopy]() {
        QueueLog("ExchangeToken thread: InternetOpenA...");
        HINTERNET hInet = InternetOpenA("Discordant/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
        if (!hInet) { QueueLog("ExchangeToken: InternetOpenA failed"); m_tokenExchangeFailed = true; return; }

        QueueLog("ExchangeToken thread: InternetConnectA to streamkit.discord.com...");
        HINTERNET hConn = InternetConnectA(hInet, "streamkit.discord.com",
            INTERNET_DEFAULT_HTTPS_PORT, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
        if (!hConn) { QueueLog("ExchangeToken: InternetConnectA failed"); InternetCloseHandle(hInet); m_tokenExchangeFailed = true; return; }

        const char* acceptTypes[] = {"application/json", NULL};
        HINTERNET hReq = HttpOpenRequestA(hConn, "POST", "/overlay/token",
            NULL, NULL, acceptTypes, INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_CACHE_WRITE, 0);
        if (!hReq) {
            QueueLog("ExchangeToken: HttpOpenRequestA failed");
            InternetCloseHandle(hConn);
            InternetCloseHandle(hInet);
            m_tokenExchangeFailed = true;
            return;
        }

        std::string headers = "Content-Type: application/json\r\n";
        json body;
        body["code"] = codeCopy;
        std::string bodyStr = body.dump();

        QueueLog("ExchangeToken thread: HttpSendRequestA...");
        BOOL sent = HttpSendRequestA(hReq, headers.c_str(), (DWORD)headers.size(),
            (LPVOID)bodyStr.c_str(), (DWORD)bodyStr.size());
        if (!sent) {
            QueueLog("ExchangeToken: HttpSendRequestA failed");
            InternetCloseHandle(hReq);
            InternetCloseHandle(hConn);
            InternetCloseHandle(hInet);
            m_tokenExchangeFailed = true;
            return;
        }

        char buf[4096];
        DWORD bytesRead = 0;
        std::string response;
        while (InternetReadFile(hReq, buf, sizeof(buf), &bytesRead) && bytesRead > 0) {
            response.append(buf, bytesRead);
        }

        InternetCloseHandle(hReq);
        InternetCloseHandle(hConn);
        InternetCloseHandle(hInet);

        QueueLog("ExchangeToken response: " + response.substr(0, 200));

        try {
            auto rj = json::parse(response);
            if (rj.contains("access_token")) {
                m_pendingAccessToken = rj["access_token"].get<std::string>();
                QueueLog("ExchangeToken: got access token");
            } else {
                QueueLog("ExchangeToken: no access_token in response");
                m_tokenExchangeFailed = true;
            }
        } catch (...) {
            QueueLog("ExchangeToken: JSON parse failed");
            m_tokenExchangeFailed = true;
        }
    }).detach();
}

void DiscordClient::Authenticate() {
    m_state.store(DiscordState::Authenticating);
    json args;
    args["access_token"] = m_accessToken;

    json j;
    j["cmd"] = "AUTHENTICATE";
    j["args"] = args;
    j["nonce"] = "discordant_authenticate";
    m_ws.SendText(j.dump());
}

void DiscordClient::SubscribeVoiceChannel(const std::string& channelId) {
    const char* events[] = {
        "VOICE_STATE_CREATE",
        "VOICE_STATE_UPDATE",
        "VOICE_STATE_DELETE",
        "SPEAKING_START",
        "SPEAKING_STOP"
    };
    for (const char* ev : events) {
        json j;
        j["cmd"] = "SUBSCRIBE";
        j["args"] = {{"channel_id", channelId}};
        j["evt"] = ev;
        j["nonce"] = channelId;
        m_ws.SendText(j.dump());
    }
}

void DiscordClient::UnsubscribeVoiceChannel(const std::string& channelId) {
    const char* events[] = {
        "VOICE_STATE_CREATE",
        "VOICE_STATE_UPDATE",
        "VOICE_STATE_DELETE",
        "SPEAKING_START",
        "SPEAKING_STOP"
    };
    for (const char* ev : events) {
        json j;
        j["cmd"] = "UNSUBSCRIBE";
        j["args"] = {{"channel_id", channelId}};
        j["evt"] = ev;
        j["nonce"] = channelId;
        m_ws.SendText(j.dump());
    }
}

void DiscordClient::SubscribeGlobalEvents() {
    const char* events[] = {
        "VOICE_CHANNEL_SELECT",
        "VOICE_SETTINGS_UPDATE"
    };
    for (const char* ev : events) {
        json j;
        j["cmd"] = "SUBSCRIBE";
        j["args"] = json::object();
        j["evt"] = ev;
        j["nonce"] = ev;
        m_ws.SendText(j.dump());
    }
}

void DiscordClient::RequestSelectedVoiceChannel() {
    json j;
    j["cmd"] = "GET_SELECTED_VOICE_CHANNEL";
    j["args"] = json::object();
    j["nonce"] = "get_voice_channel";
    m_ws.SendText(j.dump());
}

void DiscordClient::UpdateUser(const std::string& id, const VoiceUser& partial) {
    // Must be called with m_userMutex held
    for (auto& u : m_users) {
        if (u.id == id) {
            if (!partial.username.empty()) u.username = partial.username;
            if (!partial.avatar.empty()) u.avatar = partial.avatar;
            if (!partial.nick.empty()) u.nick = partial.nick;
            u.mute = partial.mute;
            u.deaf = partial.deaf;
            // Don't overwrite speaking from voice state events (speaking comes from SPEAKING_START/STOP)
            return;
        }
    }
    // New user
    m_users.push_back(partial);
}

void DiscordClient::RemoveUser(const std::string& id) {
    // Must be called with m_userMutex held
    m_users.erase(
        std::remove_if(m_users.begin(), m_users.end(),
            [&id](const VoiceUser& u) { return u.id == id; }),
        m_users.end());
}
