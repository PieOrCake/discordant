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
    if (m_networkThread.joinable())
        m_networkThread.join();
    if (m_tokenThread.joinable())
        m_tokenThread.join();
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
    }
}

std::vector<VoiceUser> DiscordClient::GetVoiceUsers() {
    std::lock_guard<std::mutex> lock(m_userMutex);
    return m_users;
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

VoiceSnapshot DiscordClient::GetVoiceSnapshot() {
    std::lock_guard<std::mutex> lock(m_userMutex);
    VoiceSnapshot snap;
    snap.users = m_users;
    snap.channelName = m_channelName;
    snap.channelIconId = m_channelIconId;
    snap.channelIconName = m_channelIconName;
    return snap;
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
    QueueLog(std::string("Network thread started; cached access token: ")
             + (m_accessToken.empty() ? "none" : "present"));
    try {
        NetworkLoop();
    } catch (...) {
        // Never let an exception escape a thread — that kills the game
        m_state.store(DiscordState::Disconnected);
        QueueLog("Network thread aborted after an unhandled error");
    }
}

void DiscordClient::NetworkLoop() {
    while (m_running.load()) {
        // Handle token exchange completion
        if (m_tokenExchangePending.load()) {
            if (m_tokenExchangeFailed.load()) {
                m_tokenExchangePending.store(false);
                m_tokenExchangeFailed.store(false);
                m_state.store(DiscordState::Disconnected);
                m_reconnectAt = GetTickCount64() + 30000;
                Sleep(100);
                continue;
            }
            std::string token;
            {
                std::lock_guard<std::mutex> lock(m_tokenMutex);
                token = m_pendingAccessToken;
            }
            if (!token.empty()) {
                m_accessToken = token;
                {
                    std::lock_guard<std::mutex> lock(m_tokenMutex);
                    m_pendingAccessToken.clear();
                }
                m_tokenExchangePending.store(false);
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
            int refusedCount = 0;
            while (m_currentPort <= RPC_PORT_MAX && m_running.load()) {
                std::string path = "/?v=1&client_id=";
                path += STREAMKIT_CLIENT_ID;
                if (m_ws.Connect("127.0.0.1", m_currentPort, path, "http://localhost:3000")) {
                    QueueLog("WebSocket connected on port " + std::to_string(m_currentPort)
                             + ", waiting for Discord READY");
                    m_waitingForReadyAt = GetTickCount64();
                    m_readyTimeoutLogged = false;
                    m_state.store(DiscordState::WaitingForReady);
                    break;
                }
                // A refused port just means Discord isn't on it — expected, and
                // noisy to log ten times. Anything else means something answered
                // and turned us away, which is always worth a line.
                if (m_ws.LastErrorWasRefused()) {
                    refusedCount++;
                } else {
                    QueueLog("Port " + std::to_string(m_currentPort) + ": " + m_ws.GetLastError());
                }
                m_currentPort++;
            }
            if (m_state.load() != DiscordState::WaitingForReady) {
                // All ports failed — schedule reconnect
                QueueLog("No Discord RPC server found on ports "
                         + std::to_string(RPC_PORT_MIN) + "-" + std::to_string(RPC_PORT_MAX)
                         + " (" + std::to_string(refusedCount) + " refused/silent). "
                         "Is the Discord desktop client running? Retrying in 10s.");
                m_state.store(DiscordState::Disconnected);
                m_reconnectAt = GetTickCount64() + 10000;
            }
            Sleep(16);
            continue;
        }
        case DiscordState::WaitingForReady:
            if (!m_readyTimeoutLogged && m_waitingForReadyAt > 0
                && GetTickCount64() - m_waitingForReadyAt > 10000) {
                m_readyTimeoutLogged = true;
                QueueLog("Socket is open but Discord has not sent READY after 10s. "
                         "The RPC handshake was accepted but the client is not responding.");
            }
            break;
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
            QueueLog("WebSocket dropped while in state '" + GetStatusText()
                     + "'. Reconnecting in 10s.");
            m_state.store(DiscordState::Disconnected);
            m_reconnectAt = GetTickCount64() + 10000;
            m_currentVoice.clear();
            {
                std::lock_guard<std::mutex> lock(m_userMutex);
                m_users.clear();
                m_channelName.clear();
                m_channelIconId.clear();
                m_channelIconName.clear();
            }
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

// Returns a pointer to an object-valued member, or nullptr if absent/not an object.
static const json* ObjectMember(const json& j, const char* key) {
    auto it = j.find(key);
    return (it != j.end() && it->is_object()) ? &(*it) : nullptr;
}

static std::string StringMember(const json& j, const char* key) {
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

static VoiceUser ParseVoiceUserFromJSON(const nlohmann::json& d) {
    VoiceUser u;
    if (const json* user = ObjectMember(d, "user")) {
        u.id       = StringMember(*user, "id");
        u.username = StringMember(*user, "username");
        u.avatar   = StringMember(*user, "avatar");
    }
    u.nick = StringMember(d, "nick");
    if (const json* vs = ObjectMember(d, "voice_state")) {
        u.mute = vs->value("mute", false) || vs->value("self_mute", false) || vs->value("suppress", false);
        u.deaf = vs->value("deaf", false) || vs->value("self_deaf", false);
    }
    return u;
}

void DiscordClient::HandleMessage(const std::string& message) {
    try {
        HandleMessageImpl(message);
    } catch (const std::exception& e) {
        // An escaped exception on this thread would terminate the whole game
        QueueLog(std::string("HandleMessage: unhandled error: ") + e.what());
    } catch (...) {
        QueueLog("HandleMessage: unhandled error");
    }
}

void DiscordClient::HandleMessageImpl(const std::string& message) {

    json j;
    try {
        j = json::parse(message);
    } catch (...) {
        QueueLog("HandleMessage: JSON parse failed");
        return;
    }

    std::string cmd = StringMember(j, "cmd");
    std::string evt = StringMember(j, "evt");

    // Discord also replies with error frames that carry no "data" object at all,
    // so every handler below must tolerate it being missing.
    const json* data = ObjectMember(j, "data");

    // Any error frame is worth reporting verbatim — Discord's code/message pair
    // says exactly why it turned us away (bad scope, rate limit, expired token).
    if (evt == "ERROR") {
        std::string detail;
        if (data) {
            detail = "code " + std::to_string(data->value("code", 0))
                   + ": " + StringMember(*data, "message");
        } else {
            detail = "no detail supplied";
        }
        QueueLog("Discord returned an error for '" + (cmd.empty() ? "(no cmd)" : cmd)
                 + "' — " + detail);
    }

    // --- DISPATCH events ---
    if (cmd == "DISPATCH") {
        if (evt == "READY") {
            // Connection established, start auth
            if (!m_accessToken.empty()) {
                QueueLog("READY received. A cached access token exists ("
                         + std::to_string(m_accessToken.size())
                         + " chars), authenticating with it — no Discord prompt will appear.");
                Authenticate();
            } else {
                QueueLog("READY received. No cached token, requesting authorization "
                         "— Discord should now show a StreamKit prompt.");
                StartAuthorize();
            }
            return;
        }

        if (evt == "VOICE_STATE_UPDATE") {
            if (!data) return;
            std::lock_guard<std::mutex> lock(m_userMutex);
            VoiceUser u = ParseVoiceUserFromJSON(*data);
            if (!u.id.empty()) UpdateUser(u.id, u);
            return;
        }

        if (evt == "VOICE_STATE_CREATE") {
            if (!data) return;
            std::lock_guard<std::mutex> lock(m_userMutex);
            VoiceUser u = ParseVoiceUserFromJSON(*data);
            if (u.id.empty()) return;
            UpdateUser(u.id, u);
            if (u.id == m_userId)
                RequestSelectedVoiceChannel();
            return;
        }

        if (evt == "VOICE_STATE_DELETE") {
            if (!data) return;
            std::lock_guard<std::mutex> lock(m_userMutex);
            // Discord sends the departing user as either a nested object or a flat id
            std::string uid;
            if (const json* user = ObjectMember(*data, "user"))
                uid = StringMember(*user, "id");
            if (uid.empty()) uid = StringMember(*data, "user_id");
            if (uid.empty()) return;
            RemoveUser(uid);
            if (uid == m_userId) {
                m_users.clear();
                RequestSelectedVoiceChannel();
            }
            return;
        }

        if (evt == "SPEAKING_START" || evt == "SPEAKING_STOP") {
            if (!data) return;
            bool speaking = (evt == "SPEAKING_START");
            std::lock_guard<std::mutex> lock(m_userMutex);
            std::string uid = StringMember(*data, "user_id");
            if (uid.empty()) return;
            for (auto& u : m_users) {
                if (u.id == uid) { u.speaking = speaking; break; }
            }
            return;
        }

        if (evt == "VOICE_CHANNEL_SELECT") {
            if (!data) return;
            std::string chId = StringMember(*data, "channel_id");
            if (!chId.empty()) {
                if (chId != m_currentVoice) {
                    if (!m_currentVoice.empty()) {
                        UnsubscribeVoiceChannel(m_currentVoice);
                    }
                    {
                        std::lock_guard<std::mutex> lock(m_userMutex);
                        m_users.clear();
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
                    m_channelName.clear();
                    m_channelIconId.clear();
                    m_channelIconName.clear();
                }
            }
            return;
        }

        if (evt == "VOICE_SETTINGS_UPDATE") {
            if (!data) return;
            m_selfMuted = data->value("mute", false);
            m_selfDeafened = data->value("deaf", false);
            return;
        }

        // Ignore other dispatch events
        return;
    }

    // --- Command responses ---
    if (cmd == "AUTHORIZE") {
        std::string code = data ? StringMember(*data, "code") : std::string();
        if (!code.empty()) {
            QueueLog("AUTHORIZE approved, exchanging code for a token");
            ExchangeToken(code);
        } else {
            // Authorization denied
            QueueLog("AUTHORIZE returned no code — the prompt was denied, dismissed, "
                     "or never shown. Retrying in 60s.");
            m_state.store(DiscordState::Disconnected);
            m_reconnectAt = GetTickCount64() + 60000;
        }
        return;
    }

    if (cmd == "AUTHENTICATE") {
        if (evt == "ERROR") {
            // Token expired or invalid, re-authorize
            QueueLog("Cached token was rejected, discarding it and re-authorizing");
            m_accessToken.clear();
            StartAuthorize();
            return;
        }
        if (data) {
            if (const json* user = ObjectMember(*data, "user"))
                m_userId = StringMember(*user, "id");
            QueueLog("Authenticated as user id " + (m_userId.empty() ? "(unknown)" : m_userId));
            m_state.store(DiscordState::Connected);
            SubscribeGlobalEvents();
            RequestSelectedVoiceChannel();
        } else {
            QueueLog("AUTHENTICATE reply had no data payload — stuck in Authenticating.");
        }
        return;
    }

    if (cmd == "GET_SELECTED_VOICE_CHANNEL") {
        if (!data) {
            // Not in a voice channel
            if (!m_currentVoice.empty()) {
                UnsubscribeVoiceChannel(m_currentVoice);
            }
            m_currentVoice.clear();
            {
                std::lock_guard<std::mutex> lock(m_userMutex);
                m_users.clear();
                m_channelName.clear();
            }
            return;
        }

        const json& d = *data;
        std::string chId   = StringMember(d, "id");
        std::string chName = StringMember(d, "name");

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
            if (const json* ie = ObjectMember(d, "icon_emoji")) {
                m_channelIconId   = StringMember(*ie, "id");
                m_channelIconName = StringMember(*ie, "name");
            }
            m_users.clear();

            auto vsIt = d.find("voice_states");
            if (vsIt != d.end() && vsIt->is_array()) {
                for (const auto& vs : *vsIt) {
                    if (!vs.is_object()) continue;
                    VoiceUser u = ParseVoiceUserFromJSON(vs);
                    if (!u.id.empty()) m_users.push_back(u);
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
    args["scopes"] = json::array({"rpc"});
    args["prompt"] = "consent";

    json j;
    j["cmd"] = "AUTHORIZE";
    j["args"] = args;
    j["nonce"] = "discordant_auth";
    if (!m_ws.SendText(j.dump()))
        QueueLog("Failed to send AUTHORIZE — the socket died before the prompt could be raised");
}

void DiscordClient::ExchangeToken(const std::string& code) {
    QueueLog("ExchangeToken: starting with code " + code.substr(0, 8) + "...");
    m_state.store(DiscordState::ExchangingToken);
    m_tokenExchangePending.store(true);
    m_tokenExchangeFailed.store(false);
    {
        std::lock_guard<std::mutex> lock(m_tokenMutex);
        m_pendingAccessToken.clear();
    }

    if (m_tokenThread.joinable())
        m_tokenThread.join();

    std::string codeCopy = code;
    m_tokenThread = std::thread([this, codeCopy]() {
      try {
        QueueLog("ExchangeToken thread: InternetOpenA...");
        HINTERNET hInet = InternetOpenA("Discordant/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
        if (!hInet) { QueueLog("ExchangeToken: InternetOpenA failed"); m_tokenExchangeFailed.store(true); return; }

        QueueLog("ExchangeToken thread: InternetConnectA to streamkit.discord.com...");
        HINTERNET hConn = InternetConnectA(hInet, "streamkit.discord.com",
            INTERNET_DEFAULT_HTTPS_PORT, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
        if (!hConn) { QueueLog("ExchangeToken: InternetConnectA failed"); InternetCloseHandle(hInet); m_tokenExchangeFailed.store(true); return; }

        const char* acceptTypes[] = {"application/json", NULL};
        HINTERNET hReq = HttpOpenRequestA(hConn, "POST", "/overlay/token",
            NULL, NULL, acceptTypes, INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_CACHE_WRITE, 0);
        if (!hReq) {
            QueueLog("ExchangeToken: HttpOpenRequestA failed");
            InternetCloseHandle(hConn);
            InternetCloseHandle(hInet);
            m_tokenExchangeFailed.store(true);
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
            m_tokenExchangeFailed.store(true);
            return;
        }

        char buf[4096];
        DWORD bytesRead = 0;
        std::string response;
        while (InternetReadFile(hReq, buf, sizeof(buf), &bytesRead) && bytesRead > 0)
            response.append(buf, bytesRead);

        InternetCloseHandle(hReq);
        InternetCloseHandle(hConn);
        InternetCloseHandle(hInet);

        QueueLog("ExchangeToken response: " + response.substr(0, 200));

        try {
            auto rj = json::parse(response);
            if (rj.contains("access_token")) {
                {
                    std::lock_guard<std::mutex> lock(m_tokenMutex);
                    m_pendingAccessToken = rj["access_token"].get<std::string>();
                }
                QueueLog("ExchangeToken: got access token");
            } else {
                QueueLog("ExchangeToken: no access_token in response");
                m_tokenExchangeFailed.store(true);
            }
        } catch (...) {
            QueueLog("ExchangeToken: JSON parse failed");
            m_tokenExchangeFailed.store(true);
        }
      } catch (...) {
        // Never let an exception escape a thread
        m_tokenExchangeFailed.store(true);
      }
    });
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
