#include "DiscordClient.h"
#include <nlohmann/json.hpp>
#include <windows.h>
#include <wininet.h>
#include <thread>
#include <algorithm>

using json = nlohmann::json;

DiscordClient::DiscordClient()
    : m_currentPort(RPC_PORT_MIN)
{
}

DiscordClient::~DiscordClient() {
    Disconnect();
}

void DiscordClient::Connect() {
    if (m_running.load()) {
        // Thread is alive, probably sitting out a retry delay — retry now.
        m_reconnectAt.store(0);
        return;
    }
    if (m_networkThread.joinable())
        m_networkThread.join();
    m_running.store(true);
    m_state.store(DiscordState::Connecting);
    m_currentPort = RPC_PORT_MIN;
    m_reconnectAt.store(0);
    m_networkThread = std::thread(&DiscordClient::NetworkThreadMain, this);
}

void DiscordClient::Disconnect() {
    m_running.store(false);
    if (m_networkThread.joinable())
        m_networkThread.join();
    if (m_tokenThread.joinable())
        m_tokenThread.join();
    m_ws.Close();
    m_state.store(DiscordState::Disconnected);
    m_tokenExchangePending.store(false);
    m_tokenExchangeFailed.store(false);
    m_currentVoice.clear();
    m_userId.clear();
    {
        std::lock_guard<std::mutex> lock(m_tokenMutex);
        m_pendingAccessToken.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_userMutex);
        m_users.clear();
        m_channelName.clear();
        m_channelIconId.clear();
        m_channelIconName.clear();
    }
}

void DiscordClient::SetAccessToken(const std::string& token) {
    std::lock_guard<std::mutex> lock(m_tokenMutex);
    m_accessToken = token;
}

std::string DiscordClient::GetAccessToken() const {
    std::lock_guard<std::mutex> lock(m_tokenMutex);
    return m_accessToken;
}

void DiscordClient::StoreToken(const std::string& token) {
    {
        std::lock_guard<std::mutex> lock(m_tokenMutex);
        m_accessToken = token;
    }
    m_tokenGeneration.fetch_add(1);
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
    // Logging must never be the thing that throws on a background thread.
    try {
        std::lock_guard<std::mutex> lock(m_logMutex);
        if (m_logQueue.size() < 500)
            m_logQueue.push_back(msg);
    } catch (...) {}
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

void DiscordClient::ResetConnection(uint64_t delayMs) {
    m_ws.Close();
    m_state.store(DiscordState::Disconnected);
    m_reconnectAt.store(GetTickCount64() + delayMs);
    m_currentVoice.clear();
    std::lock_guard<std::mutex> lock(m_userMutex);
    m_users.clear();
    m_channelName.clear();
    m_channelIconId.clear();
    m_channelIconName.clear();
}

void DiscordClient::NetworkThreadMain() {
    QueueLog(std::string("Network thread started; cached access token: ")
             + (GetAccessToken().empty() ? "none" : "present"));
    // An exception escaping a thread kills the game, so catch everything —
    // and rather than dying, start over after a pause.
    while (m_running.load()) {
        bool failed = false;
        try {
            NetworkLoop();
        } catch (const std::exception& e) {
            QueueLog(std::string("Network thread error, restarting in 10s: ") + e.what());
            failed = true;
        } catch (...) {
            QueueLog("Network thread error, restarting in 10s");
            failed = true;
        }
        if (failed) {
            try { ResetConnection(10000); } catch (...) {}
            Sleep(250);
        }
    }
}

void DiscordClient::NetworkLoop() {
    while (m_running.load()) {
        // Handle token exchange completion
        if (m_tokenExchangePending.load()) {
            if (m_tokenExchangeFailed.load()) {
                m_tokenExchangePending.store(false);
                m_tokenExchangeFailed.store(false);
                ResetConnection(30000);
                Sleep(100);
                continue;
            }
            std::string token;
            {
                std::lock_guard<std::mutex> lock(m_tokenMutex);
                token.swap(m_pendingAccessToken);
            }
            if (!token.empty()) {
                StoreToken(token);
                m_tokenExchangePending.store(false);
                Authenticate();
                Sleep(16);
                continue;
            }
            // Still waiting — but keep the socket serviced so Discord's pings
            // are answered and a dropped connection is noticed.
        }

        // Connection state machine
        DiscordState curState = m_state.load();
        switch (curState) {
        case DiscordState::Disconnected: {
            uint64_t reconnectAt = m_reconnectAt.load();
            if (reconnectAt > 0 && GetTickCount64() < reconnectAt) {
                Sleep(100);
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
            if (!m_running.load()) return;
            if (m_state.load() != DiscordState::WaitingForReady) {
                QueueLog("No Discord RPC server found on ports "
                         + std::to_string(RPC_PORT_MIN) + "-" + std::to_string(RPC_PORT_MAX)
                         + " (" + std::to_string(refusedCount) + " refused/silent). "
                         "Is the Discord desktop client running? Retrying in 10s.");
                ResetConnection(10000);
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
        default:
            break;
        }

        // If WebSocket disconnected unexpectedly, schedule reconnect
        if (m_ws.GetState() != WebSocket::State::Connected) {
            QueueLog("WebSocket dropped while in state '" + GetStatusText()
                     + "'. Reconnecting in 10s.");
            ResetConnection(10000);
            Sleep(100);
            continue;
        }

        // Poll for messages
        for (const auto& msg : m_ws.Poll())
            HandleMessage(msg);

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

static bool BoolMember(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_boolean() && it->get<bool>();
}

static VoiceUser ParseVoiceUserFromJSON(const nlohmann::json& d) {
    VoiceUser u;
    if (const json* user = ObjectMember(d, "user")) {
        u.id       = StringMember(*user, "id");
        u.username = StringMember(*user, "username");
        u.avatar   = StringMember(*user, "avatar");
        u.globalName = StringMember(*user, "global_name");
    }
    u.nick = StringMember(d, "nick");
    if (const json* vs = ObjectMember(d, "voice_state")) {
        u.mute = BoolMember(*vs, "mute") || BoolMember(*vs, "self_mute") || BoolMember(*vs, "suppress");
        u.deaf = BoolMember(*vs, "deaf") || BoolMember(*vs, "self_deaf");
    }
    return u;
}

void DiscordClient::HandleMessage(const std::string& message) {
    try {
        HandleMessageImpl(message);
    } catch (const std::exception& e) {
        QueueLog(std::string("HandleMessage: unhandled error: ") + e.what());
    } catch (...) {
        QueueLog("HandleMessage: unhandled error");
    }
}

void DiscordClient::HandleMessageImpl(const std::string& message) {
    json j = json::parse(message, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
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
        std::string detail = "no detail supplied";
        if (data) {
            auto code = data->find("code");
            detail = "code " + (code != data->end() && code->is_number_integer()
                                    ? std::to_string(code->get<long long>()) : std::string("?"))
                   + ": " + StringMember(*data, "message");
        }
        QueueLog("Discord returned an error for '" + (cmd.empty() ? "(no cmd)" : cmd)
                 + "' — " + detail);
    }

    // --- DISPATCH events ---
    if (cmd == "DISPATCH") {
        if (evt == "READY") {
            std::string token = GetAccessToken();
            if (!token.empty()) {
                QueueLog("READY received. A cached access token exists, "
                         "authenticating with it — no Discord prompt will appear.");
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
            VoiceUser u = ParseVoiceUserFromJSON(*data);
            if (u.id.empty()) return;
            std::lock_guard<std::mutex> lock(m_userMutex);
            UpdateUser(u.id, u);
            return;
        }

        if (evt == "VOICE_STATE_CREATE") {
            if (!data) return;
            VoiceUser u = ParseVoiceUserFromJSON(*data);
            if (u.id.empty()) return;
            {
                std::lock_guard<std::mutex> lock(m_userMutex);
                UpdateUser(u.id, u);
            }
            // Never send on the socket while holding the lock the render thread waits on
            if (u.id == m_userId)
                RequestSelectedVoiceChannel();
            return;
        }

        if (evt == "VOICE_STATE_DELETE") {
            if (!data) return;
            // Discord sends the departing user as either a nested object or a flat id
            std::string uid;
            if (const json* user = ObjectMember(*data, "user"))
                uid = StringMember(*user, "id");
            if (uid.empty()) uid = StringMember(*data, "user_id");
            if (uid.empty()) return;
            bool wasMe = (uid == m_userId);
            {
                std::lock_guard<std::mutex> lock(m_userMutex);
                if (wasMe) m_users.clear();
                else       RemoveUser(uid);
            }
            if (wasMe)
                RequestSelectedVoiceChannel();
            return;
        }

        if (evt == "SPEAKING_START" || evt == "SPEAKING_STOP") {
            if (!data) return;
            bool speaking = (evt == "SPEAKING_START");
            std::string uid = StringMember(*data, "user_id");
            if (uid.empty()) return;
            std::lock_guard<std::mutex> lock(m_userMutex);
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
                    if (!m_currentVoice.empty())
                        UnsubscribeVoiceChannel(m_currentVoice);
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
                if (!m_currentVoice.empty())
                    UnsubscribeVoiceChannel(m_currentVoice);
                m_currentVoice.clear();
                std::lock_guard<std::mutex> lock(m_userMutex);
                m_users.clear();
                m_channelName.clear();
                m_channelIconId.clear();
                m_channelIconName.clear();
            }
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
            QueueLog("AUTHORIZE returned no code — the prompt was denied, dismissed, "
                     "or never shown. Retrying in 60s.");
            ResetConnection(60000);
        }
        return;
    }

    if (cmd == "AUTHENTICATE") {
        if (evt == "ERROR") {
            // Token expired or invalid, re-authorize
            QueueLog("Cached token was rejected, discarding it and re-authorizing");
            StoreToken(std::string());
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
            if (!m_currentVoice.empty())
                UnsubscribeVoiceChannel(m_currentVoice);
            m_currentVoice.clear();
            std::lock_guard<std::mutex> lock(m_userMutex);
            m_users.clear();
            m_channelName.clear();
            m_channelIconId.clear();
            m_channelIconName.clear();
            return;
        }

        const json& d = *data;
        std::string chId   = StringMember(d, "id");
        std::string chName = StringMember(d, "name");

        if (chId != m_currentVoice) {
            if (!m_currentVoice.empty())
                UnsubscribeVoiceChannel(m_currentVoice);
            if (!chId.empty())
                SubscribeVoiceChannel(chId);
            m_currentVoice = chId;
        }

        // Build the new state outside the lock, then swap it in
        std::string iconId, iconName;
        if (const json* ie = ObjectMember(d, "icon_emoji")) {
            iconId   = StringMember(*ie, "id");
            iconName = StringMember(*ie, "name");
        }
        std::vector<VoiceUser> users;
        auto vsIt = d.find("voice_states");
        if (vsIt != d.end() && vsIt->is_array()) {
            for (const auto& vs : *vsIt) {
                if (!vs.is_object()) continue;
                VoiceUser u = ParseVoiceUserFromJSON(vs);
                if (u.id.empty()) continue;
                u.isSelf = !m_userId.empty() && u.id == m_userId;
                users.push_back(std::move(u));
            }
        }

        std::lock_guard<std::mutex> lock(m_userMutex);
        m_channelName.swap(chName);
        m_channelIconId.swap(iconId);
        m_channelIconName.swap(iconName);
        m_users.swap(users);
        return;
    }

    // SUBSCRIBE/UNSUBSCRIBE confirmations - ignore
}

void DiscordClient::StartAuthorize() {
    QueueLog("Sending AUTHORIZE with prompt=consent");
    m_state.store(DiscordState::Authorizing);
    json j;
    j["cmd"] = "AUTHORIZE";
    j["args"] = {
        {"client_id", STREAMKIT_CLIENT_ID},
        {"scopes", json::array({"rpc"})},
        {"prompt", "consent"},
    };
    j["nonce"] = "discordant_auth";
    if (!m_ws.SendText(j.dump()))
        QueueLog("Failed to send AUTHORIZE — the socket died before the prompt could be raised");
}

void DiscordClient::ExchangeToken(const std::string& code) {
    QueueLog("Exchanging authorization code for a token");
    m_state.store(DiscordState::ExchangingToken);
    m_tokenExchangeFailed.store(false);
    {
        std::lock_guard<std::mutex> lock(m_tokenMutex);
        m_pendingAccessToken.clear();
    }
    if (m_tokenThread.joinable())
        m_tokenThread.join();
    m_tokenExchangePending.store(true);
    m_tokenThread = std::thread(&DiscordClient::TokenThreadMain, this, code);
}

// Closes a WinINet handle when it goes out of scope.
struct InetHandle {
    HINTERNET h;
    explicit InetHandle(HINTERNET handle) : h(handle) {}
    ~InetHandle() { if (h) InternetCloseHandle(h); }
    InetHandle(const InetHandle&) = delete;
    InetHandle& operator=(const InetHandle&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

void DiscordClient::TokenThreadMain(std::string code) {
    // An exception escaping a thread kills the game
    try {
        InetHandle inet(InternetOpenA("Discordant/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0));
        if (!inet) {
            QueueLog("ExchangeToken: InternetOpenA failed (error " + std::to_string(GetLastError()) + ")");
            m_tokenExchangeFailed.store(true);
            return;
        }

        // Bound every network wait, so unloading the addon (which waits for this
        // thread) can never hang the game for long.
        DWORD timeoutMs = 10000;
        DWORD retries = 1;
        InternetSetOptionA(inet.h, INTERNET_OPTION_CONNECT_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
        InternetSetOptionA(inet.h, INTERNET_OPTION_SEND_TIMEOUT,    &timeoutMs, sizeof(timeoutMs));
        InternetSetOptionA(inet.h, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
        InternetSetOptionA(inet.h, INTERNET_OPTION_CONNECT_RETRIES, &retries,   sizeof(retries));

        InetHandle conn(InternetConnectA(inet.h, "streamkit.discord.com",
            INTERNET_DEFAULT_HTTPS_PORT, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0));
        if (!conn) {
            QueueLog("ExchangeToken: InternetConnectA failed (error " + std::to_string(GetLastError()) + ")");
            m_tokenExchangeFailed.store(true);
            return;
        }

        const char* acceptTypes[] = {"application/json", NULL};
        InetHandle req(HttpOpenRequestA(conn.h, "POST", "/overlay/token",
            NULL, NULL, acceptTypes, INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_CACHE_WRITE, 0));
        if (!req) {
            QueueLog("ExchangeToken: HttpOpenRequestA failed (error " + std::to_string(GetLastError()) + ")");
            m_tokenExchangeFailed.store(true);
            return;
        }

        static const char headers[] = "Content-Type: application/json\r\n";
        std::string bodyStr = json{{"code", code}}.dump();
        if (!HttpSendRequestA(req.h, headers, (DWORD)(sizeof(headers) - 1),
                              (LPVOID)bodyStr.c_str(), (DWORD)bodyStr.size())) {
            QueueLog("ExchangeToken: request failed (error " + std::to_string(GetLastError()) + ")");
            m_tokenExchangeFailed.store(true);
            return;
        }

        DWORD status = 0, statusLen = sizeof(status);
        HttpQueryInfoA(req.h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &statusLen, NULL);

        char buf[4096];
        DWORD bytesRead = 0;
        std::string response;
        while (m_running.load() && response.size() < 64 * 1024
               && InternetReadFile(req.h, buf, sizeof(buf), &bytesRead) && bytesRead > 0)
            response.append(buf, bytesRead);

        json rj = json::parse(response, nullptr, false);
        auto tok = rj.is_object() ? rj.find("access_token") : rj.end();
        if (rj.is_object() && tok != rj.end() && tok->is_string() && !tok->get<std::string>().empty()) {
            {
                std::lock_guard<std::mutex> lock(m_tokenMutex);
                m_pendingAccessToken = tok->get<std::string>();
            }
            QueueLog("ExchangeToken: got access token (HTTP " + std::to_string(status) + ")");
        } else {
            // The body has no token in it here, so it is safe to log
            QueueLog("ExchangeToken: no access token in reply (HTTP " + std::to_string(status)
                     + "): " + response.substr(0, 200));
            m_tokenExchangeFailed.store(true);
        }
    } catch (...) {
        m_tokenExchangeFailed.store(true);
    }
}

void DiscordClient::Authenticate() {
    m_state.store(DiscordState::Authenticating);
    json j;
    j["cmd"] = "AUTHENTICATE";
    j["args"] = {{"access_token", GetAccessToken()}};
    j["nonce"] = "discordant_authenticate";
    m_ws.SendText(j.dump());
}

static const char* const kVoiceEvents[] = {
    "VOICE_STATE_CREATE",
    "VOICE_STATE_UPDATE",
    "VOICE_STATE_DELETE",
    "SPEAKING_START",
    "SPEAKING_STOP"
};

void DiscordClient::SubscribeVoiceChannel(const std::string& channelId) {
    for (const char* ev : kVoiceEvents) {
        json j;
        j["cmd"] = "SUBSCRIBE";
        j["args"] = {{"channel_id", channelId}};
        j["evt"] = ev;
        j["nonce"] = channelId;
        m_ws.SendText(j.dump());
    }
}

void DiscordClient::UnsubscribeVoiceChannel(const std::string& channelId) {
    for (const char* ev : kVoiceEvents) {
        json j;
        j["cmd"] = "UNSUBSCRIBE";
        j["args"] = {{"channel_id", channelId}};
        j["evt"] = ev;
        j["nonce"] = channelId;
        m_ws.SendText(j.dump());
    }
}

void DiscordClient::SubscribeGlobalEvents() {
    json j;
    j["cmd"] = "SUBSCRIBE";
    j["args"] = json::object();
    j["evt"] = "VOICE_CHANNEL_SELECT";
    j["nonce"] = "VOICE_CHANNEL_SELECT";
    m_ws.SendText(j.dump());
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
            if (!partial.globalName.empty()) u.globalName = partial.globalName;
            u.mute = partial.mute;
            u.deaf = partial.deaf;
            // Don't overwrite speaking from voice state events (speaking comes from SPEAKING_START/STOP)
            return;
        }
    }
    // New user
    m_users.push_back(partial);
    m_users.back().isSelf = !m_userId.empty() && id == m_userId;
}

void DiscordClient::RemoveUser(const std::string& id) {
    // Must be called with m_userMutex held
    m_users.erase(
        std::remove_if(m_users.begin(), m_users.end(),
            [&id](const VoiceUser& u) { return u.id == id; }),
        m_users.end());
}
