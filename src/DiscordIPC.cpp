#include "DiscordIPC.h"
#include <nlohmann/json.hpp>
#include <string>
#include <cstdint>

using json = nlohmann::json;

static constexpr uint32_t OP_HANDSHAKE = 0;
static constexpr uint32_t OP_FRAME     = 1;
static constexpr uint32_t OP_CLOSE     = 2;

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
    if (appId.empty()) return false;

    wchar_t pipeName[] = L"\\\\.\\pipe\\discord-ipc-0";
    const size_t digitIdx = (sizeof(pipeName) / sizeof(wchar_t)) - 2;
    for (int i = 0; i < 10 && m_running.load(); ++i) {
        pipeName[digitIdx] = L'0' + i;
        m_pipe = CreateFileW(pipeName, GENERIC_READ | GENERIC_WRITE,
                             0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (m_pipe != INVALID_HANDLE_VALUE) break;
    }
    if (m_pipe == INVALID_HANDLE_VALUE) return false;

    json hs;
    hs["v"]         = 1;
    hs["client_id"] = appId;
    if (!SendFrame(OP_HANDSHAKE, hs.dump())) {
        Disconnect();
        return false;
    }

    Sleep(200);
    uint32_t op = 0;
    std::string resp;
    if (!ReadFrame(op, resp)) {
        return true; // optimistically continue; READY may come later
    }
    try {
        auto rj = json::parse(resp);
        if (rj.value("evt", "") != "READY") {
            Disconnect();
            return false;
        }
    } catch (...) {
        Disconnect();
        return false;
    }
    return true;
}

void DiscordIPC::Disconnect() {
    if (m_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }
}

void DiscordIPC::ThreadMain() {
    while (m_running.load()) {
        if (m_reconnect.load()) {
            m_reconnect.store(false);
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
                Disconnect();
                m_reconnectAt = GetTickCount64() + 5000;
                break;
            }
        }

        // Check pipe health
        if (m_pipe != INVALID_HANDLE_VALUE) {
            DWORD flags = 0;
            if (!GetNamedPipeHandleStateA(m_pipe, &flags, nullptr, nullptr, nullptr, nullptr, 0)) {
                Disconnect();
                m_reconnectAt = GetTickCount64() + 5000;
            }
        }

        // Send pending activity
        if (m_activityPending.load() && m_pipe != INVALID_HANDLE_VALUE) {
            std::string act;
            {
                std::lock_guard<std::mutex> lock(m_activityMutex);
                act = m_pendingActivity;
                m_pendingActivity.clear();
                m_activityPending.store(false);
            }
            if (!SendFrame(OP_FRAME, act)) {
                Disconnect();
                m_reconnectAt = GetTickCount64() + 5000;
            }
        }

        Sleep(16);
    }

    Disconnect();
}
