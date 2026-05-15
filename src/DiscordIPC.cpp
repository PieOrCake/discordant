#include "DiscordIPC.h"
#include <nlohmann/json.hpp>
#include <string>
#include <cstdint>

using json = nlohmann::json;

static constexpr uint32_t OP_HANDSHAKE = 0;
static constexpr uint32_t OP_FRAME     = 1;
static constexpr uint32_t OP_CLOSE     = 2;

void DiscordIPC::QueueLog(const std::string& msg) {
    std::lock_guard<std::mutex> lock(m_logMutex);
    m_logQueue.push_back(msg);
}

std::vector<std::string> DiscordIPC::DrainLogQueue() {
    std::lock_guard<std::mutex> lock(m_logMutex);
    std::vector<std::string> out;
    out.swap(m_logQueue);
    return out;
}

DiscordIPC::DiscordIPC() {}

DiscordIPC::~DiscordIPC() {
    Stop();
}

void DiscordIPC::Start(const std::string& appId) {
    {
        std::lock_guard<std::mutex> lock(m_appIdMutex);
        m_appId = appId;
    }
    m_running.store(true);
    m_thread = std::thread(&DiscordIPC::ThreadMain, this);
}

void DiscordIPC::Stop() {
    m_running.store(false);
    if (m_thread.joinable())
        m_thread.join();
    Disconnect();
}

void DiscordIPC::SetAppId(const std::string& appId) {
    std::lock_guard<std::mutex> lock(m_appIdMutex);
    if (m_appId != appId) {
        m_appId = appId;
        m_reconnect.store(true);
    }
}

void DiscordIPC::SetActivity(const std::string& activityJson) {
    std::lock_guard<std::mutex> lock(m_activityMutex);
    m_pendingActivity = activityJson;
    m_activityPending.store(true);
}

void DiscordIPC::ClearActivity() {
    json j;
    j["cmd"]              = "SET_ACTIVITY";
    j["nonce"]            = "rp_clear";
    j["args"]["pid"]      = (int)GetCurrentProcessId();
    j["args"]["activity"] = nullptr;
    SetActivity(j.dump());
}

bool DiscordIPC::SendFrame(uint32_t opcode, const std::string& payload) {
    if (m_pipe == INVALID_HANDLE_VALUE) return false;
    uint32_t len = (uint32_t)payload.size();
    DWORD written = 0;
    uint32_t header[2] = {opcode, len};
    if (!WriteFile(m_pipe, header, sizeof(header), &written, nullptr) || written != sizeof(header))
        return false;
    if (len > 0) {
        if (!WriteFile(m_pipe, payload.data(), len, &written, nullptr) || written != len)
            return false;
    }
    return true;
}

bool DiscordIPC::ReadFrame(uint32_t& opcode, std::string& payload) {
    if (m_pipe == INVALID_HANDLE_VALUE) return false;
    DWORD available = 0;
    if (!PeekNamedPipe(m_pipe, nullptr, 0, nullptr, &available, nullptr) || available < 8)
        return false;
    uint32_t header[2] = {0, 0};
    DWORD read = 0;
    if (!ReadFile(m_pipe, header, sizeof(header), &read, nullptr) || read != sizeof(header))
        return false;
    opcode = header[0];
    uint32_t len = header[1];
    payload.resize(len);
    if (len > 0) {
        if (!ReadFile(m_pipe, payload.data(), len, &read, nullptr) || read != len)
            return false;
    }
    return true;
}

bool DiscordIPC::Connect() {
    std::string appId;
    {
        std::lock_guard<std::mutex> lock(m_appIdMutex);
        appId = m_appId;
    }
    if (appId.empty()) {
        QueueLog("IPC: Connect() called with empty appId, skipping");
        return false;
    }

    QueueLog("IPC: scanning discord-ipc-0..9 (appId=" + appId + ")");

    wchar_t pipeName[] = L"\\\\.\\pipe\\discord-ipc-0";
    const size_t digitIdx = (sizeof(pipeName) / sizeof(wchar_t)) - 2;
    int foundPipe = -1;
    for (int i = 0; i < 10 && m_running.load(); ++i) {
        pipeName[digitIdx] = L'0' + i;
        m_pipe = CreateFileW(pipeName, GENERIC_READ | GENERIC_WRITE,
                             0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (m_pipe != INVALID_HANDLE_VALUE) { foundPipe = i; break; }
    }
    if (m_pipe == INVALID_HANDLE_VALUE) {
        QueueLog("IPC: no discord-ipc pipe found (GetLastError=" + std::to_string(GetLastError()) + ")");
        return false;
    }
    QueueLog("IPC: opened discord-ipc-" + std::to_string(foundPipe));

    json hs;
    hs["v"]         = 1;
    hs["client_id"] = appId;
    if (!SendFrame(OP_HANDSHAKE, hs.dump())) {
        QueueLog("IPC: handshake write failed (GetLastError=" + std::to_string(GetLastError()) + ")");
        Disconnect();
        return false;
    }
    QueueLog("IPC: handshake sent, waiting for READY");
    return true;
}

void DiscordIPC::Disconnect() {
    m_ready.store(false);
    if (m_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }
}

void DiscordIPC::ThreadMain() {
    QueueLog("IPC: thread started");
    while (m_running.load()) {
        if (m_reconnect.load()) {
            QueueLog("IPC: reconnect triggered (appId changed)");
            m_reconnect.store(false);
            m_ready.store(false);
            Disconnect();
            m_reconnectAt = 0;
        }

        if (m_pipe == INVALID_HANDLE_VALUE) {
            uint64_t now = GetTickCount64();
            if (m_reconnectAt > 0 && now < m_reconnectAt) {
                Sleep(500);
                continue;
            }
            if (!Connect()) {
                QueueLog("IPC: connect failed, retrying in 10s");
                m_ready.store(false);
                m_reconnectAt = GetTickCount64() + 10000;
                Sleep(500);
                continue;
            }
        }

        // Drain incoming frames
        uint32_t op = 0;
        std::string payload;
        while (ReadFrame(op, payload)) {
            if (op == OP_CLOSE) {
                QueueLog("IPC: OP_CLOSE received from Discord, reconnecting in 5s");
                m_ready.store(false);
                Disconnect();
                m_reconnectAt = GetTickCount64() + 5000;
                break;
            }
            if (!m_ready.load() && op == OP_FRAME) {
                try {
                    auto rj = json::parse(payload);
                    std::string evt = rj.value("evt", "");
                    if (evt == "READY") {
                        QueueLog("IPC: READY received — connection established");
                        m_ready.store(true);
                    } else {
                        QueueLog("IPC: unexpected frame before READY: evt='" + evt + "'");
                    }
                } catch (...) {
                    QueueLog("IPC: failed to parse pre-READY frame");
                }
            }
        }

        // Check pipe health
        if (m_pipe != INVALID_HANDLE_VALUE) {
            DWORD flags = 0;
            if (!GetNamedPipeHandleStateA(m_pipe, &flags, nullptr, nullptr, nullptr, nullptr, 0)) {
                QueueLog("IPC: pipe health check failed (GetLastError=" + std::to_string(GetLastError()) + "), reconnecting in 5s");
                m_ready.store(false);
                Disconnect();
                m_reconnectAt = GetTickCount64() + 5000;
            }
        }

        // Send pending activity (only after READY)
        if (m_activityPending.load() && m_ready.load() && m_pipe != INVALID_HANDLE_VALUE) {
            std::string act;
            {
                std::lock_guard<std::mutex> lock(m_activityMutex);
                act = m_pendingActivity;
                m_pendingActivity.clear();
                m_activityPending.store(false);
            }
            QueueLog("IPC: sending activity frame");
            if (!SendFrame(OP_FRAME, act)) {
                QueueLog("IPC: activity frame send failed (GetLastError=" + std::to_string(GetLastError()) + "), reconnecting in 5s");
                Disconnect();
                m_reconnectAt = GetTickCount64() + 5000;
            }
        }

        Sleep(16);
    }

    QueueLog("IPC: thread stopped");
    Disconnect();
}
