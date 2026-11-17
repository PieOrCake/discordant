#include "DiscordIPC.h"
#include <nlohmann/json.hpp>
#include <string>
#include <cstdint>
#include <cstring>

using json = nlohmann::json;

static constexpr uint32_t OP_HANDSHAKE = 0;
static constexpr uint32_t OP_FRAME     = 1;
static constexpr uint32_t OP_CLOSE     = 2;

// Longest we will wait on a single pipe read or write. Discord answers in
// milliseconds; this only matters if it hangs, and keeps shutdown from hanging with it.
static constexpr DWORD IO_TIMEOUT_MS = 2000;

// Discord's largest RPC frames are a few KB; anything beyond this means the
// stream has desynced and the length is garbage.
static constexpr uint32_t MAX_FRAME_PAYLOAD = 1u * 1024 * 1024;

void DiscordIPC::QueueLog(const std::string& msg) {
    // Logging must never be the thing that throws on a background thread.
    try {
        std::lock_guard<std::mutex> lock(m_logMutex);
        if (m_logQueue.size() < 500)
            m_logQueue.push_back(msg);
    } catch (...) {}
}

std::vector<std::string> DiscordIPC::DrainLogQueue() {
    std::lock_guard<std::mutex> lock(m_logMutex);
    std::vector<std::string> out;
    out.swap(m_logQueue);
    return out;
}

DiscordIPC::DiscordIPC() {
    m_ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

DiscordIPC::~DiscordIPC() {
    Stop();
    if (m_ioEvent) CloseHandle(m_ioEvent);
}

void DiscordIPC::Start(const std::string& appId) {
    {
        std::lock_guard<std::mutex> lock(m_appIdMutex);
        m_appId = appId;
    }
    if (m_thread.joinable()) return;
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

// One overlapped read or write with a timeout. A plain blocking call on a pipe
// can wait forever if Discord stops reading, which would hang the game on exit.
bool DiscordIPC::PipeIo(bool write, void* buf, DWORD len) {
    if (m_pipe == INVALID_HANDLE_VALUE || !m_ioEvent) return false;
    if (len == 0) return true;

    OVERLAPPED ov{};
    ov.hEvent = m_ioEvent;
    ResetEvent(m_ioEvent);

    DWORD done = 0;
    BOOL ok = write ? WriteFile(m_pipe, buf, len, &done, &ov)
                    : ReadFile(m_pipe, buf, len, &done, &ov);
    if (!ok) {
        if (GetLastError() != ERROR_IO_PENDING) return false;
        if (WaitForSingleObject(m_ioEvent, IO_TIMEOUT_MS) != WAIT_OBJECT_0) {
            CancelIoEx(m_pipe, &ov);
            // Wait for the cancel to land: 'ov' and 'buf' must outlive the I/O
            GetOverlappedResult(m_pipe, &ov, &done, TRUE);
            return false;
        }
        if (!GetOverlappedResult(m_pipe, &ov, &done, FALSE)) return false;
    }
    return done == len;
}

bool DiscordIPC::SendFrame(uint32_t opcode, const std::string& payload) {
    // Header and payload in one write, so a frame is never left half-sent
    // between two calls.
    std::string frame(8 + payload.size(), '\0');
    uint32_t header[2] = {opcode, (uint32_t)payload.size()};
    memcpy(&frame[0], header, sizeof(header));
    if (!payload.empty())
        memcpy(&frame[8], payload.data(), payload.size());
    return PipeIo(true, &frame[0], (DWORD)frame.size());
}

bool DiscordIPC::ReadFrame(uint32_t& opcode, std::string& payload) {
    if (m_pipe == INVALID_HANDLE_VALUE) return false;

    // Peek the header without consuming it, so we only commit to a read once
    // the entire frame has actually arrived.
    uint32_t header[2] = {0, 0};
    DWORD peeked = 0, available = 0;
    if (!PeekNamedPipe(m_pipe, header, sizeof(header), &peeked, &available, nullptr))
        return false;
    if (peeked < sizeof(header)) return false;

    opcode = header[0];
    uint32_t len = header[1];

    if (len > MAX_FRAME_PAYLOAD) {
        QueueLog("IPC: frame length " + std::to_string(len) + " out of range, dropping connection");
        Disconnect();
        m_reconnectAt = GetTickCount64() + 5000;
        return false;
    }
    if (available < sizeof(header) + len) return false;  // frame still in flight

    if (!PipeIo(false, header, sizeof(header)))
        return false;
    payload.assign(len, '\0');
    if (len > 0 && !PipeIo(false, &payload[0], len))
        return false;
    return true;
}

void DiscordIPC::LogConnectProblem(const std::string& msg) {
    if (msg == m_lastConnectProblem) return;
    m_lastConnectProblem = msg;
    QueueLog(msg + " Retrying every 10s; this message repeats only if the cause changes.");
}

bool DiscordIPC::Connect() {
    std::string appId;
    {
        std::lock_guard<std::mutex> lock(m_appIdMutex);
        appId = m_appId;
    }
    if (appId.empty()) {
        LogConnectProblem("IPC: no Application ID set.");
        return false;
    }

    wchar_t pipeName[] = L"\\\\.\\pipe\\discord-ipc-0";
    const size_t digitIdx = (sizeof(pipeName) / sizeof(wchar_t)) - 2;
    int foundPipe = -1;
    // Remember the first error that isn't "doesn't exist" — a pipe that exists
    // but refuses us is the useful clue, and must not be masked by later pipes.
    DWORD firstErr = 0;
    int firstErrPipe = -1;
    for (int i = 0; i < 10 && m_running.load(); ++i) {
        pipeName[digitIdx] = (wchar_t)(L'0' + i);
        m_pipe = CreateFileW(pipeName, GENERIC_READ | GENERIC_WRITE,
                             0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (m_pipe != INVALID_HANDLE_VALUE) { foundPipe = i; break; }
        DWORD err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND && firstErr == 0) {
            firstErr = err;
            firstErrPipe = i;
        }
    }
    if (m_pipe == INVALID_HANDLE_VALUE) {
        std::string pipe = "discord-ipc-" + std::to_string(firstErrPipe);
        if (firstErr == 0)
            LogConnectProblem("IPC: no discord-ipc pipe found. Is the Discord desktop client running?");
        else if (firstErr == ERROR_ACCESS_DENIED)
            LogConnectProblem("IPC: " + pipe + " exists but access was denied (error 5). "
                              "Discord and Guild Wars 2 are probably running with different "
                              "permissions (one as administrator) — run both the same way.");
        else if (firstErr == ERROR_PIPE_BUSY)
            LogConnectProblem("IPC: " + pipe + " exists but is busy (error 231).");
        else
            LogConnectProblem("IPC: " + pipe + " could not be opened (error "
                              + std::to_string(firstErr) + ").");
        return false;
    }
    m_lastConnectProblem.clear();
    QueueLog("IPC: opened discord-ipc-" + std::to_string(foundPipe) + " (appId=" + appId + ")");

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

void DiscordIPC::SendPendingActivity() {
    if (!m_activityPending.load() || !m_ready.load() || m_pipe == INVALID_HANDLE_VALUE)
        return;
    std::string act;
    {
        std::lock_guard<std::mutex> lock(m_activityMutex);
        act.swap(m_pendingActivity);
        m_activityPending.store(false);
    }
    if (!SendFrame(OP_FRAME, act)) {
        QueueLog("IPC: activity frame send failed (GetLastError=" + std::to_string(GetLastError()) + "), reconnecting in 5s");
        // Put it back so it is sent after reconnecting, unless something newer arrived
        {
            std::lock_guard<std::mutex> lock(m_activityMutex);
            if (!m_activityPending.load()) {
                m_pendingActivity.swap(act);
                m_activityPending.store(true);
            }
        }
        Disconnect();
        m_reconnectAt = GetTickCount64() + 5000;
    }
}

void DiscordIPC::ThreadMain() {
    try {
        ThreadLoop();
        // Deliver a final update queued during shutdown (clearing the presence)
        SendPendingActivity();
    } catch (...) {
        // Never let an exception escape a thread — that kills the game
        QueueLog("IPC: thread aborted after an unhandled error");
    }
    Disconnect();
}

void DiscordIPC::ThreadLoop() {
    while (m_running.load()) {
        if (m_reconnect.load()) {
            QueueLog("IPC: Application ID changed, reconnecting");
            m_reconnect.store(false);
            Disconnect();
            m_reconnectAt = 0;
            m_lastConnectProblem.clear();
        }

        if (m_pipe == INVALID_HANDLE_VALUE) {
            if (m_reconnectAt > 0 && GetTickCount64() < m_reconnectAt) {
                Sleep(100);
                continue;
            }
            if (!Connect()) {
                m_reconnectAt = GetTickCount64() + 10000;
                continue;
            }
        }

        // Drain incoming frames
        uint32_t op = 0;
        std::string payload;
        while (ReadFrame(op, payload)) {
            if (op == OP_CLOSE) {
                QueueLog("IPC: Discord closed the connection, reconnecting in 5s");
                Disconnect();
                m_reconnectAt = GetTickCount64() + 5000;
                break;
            }
            if (!m_ready.load() && op == OP_FRAME) {
                json rj = json::parse(payload, nullptr, false);
                std::string evt;
                if (rj.is_object()) {
                    auto it = rj.find("evt");
                    if (it != rj.end() && it->is_string()) evt = it->get<std::string>();
                }
                if (evt == "READY") {
                    QueueLog("IPC: READY received — connection established");
                    m_ready.store(true);
                } else {
                    QueueLog("IPC: unexpected frame before READY: evt='" + evt + "'");
                }
            }
        }

        // Check pipe health
        if (m_pipe != INVALID_HANDLE_VALUE) {
            DWORD flags = 0;
            if (!GetNamedPipeHandleStateA(m_pipe, &flags, nullptr, nullptr, nullptr, nullptr, 0)) {
                QueueLog("IPC: pipe health check failed (GetLastError=" + std::to_string(GetLastError()) + "), reconnecting in 5s");
                Disconnect();
                m_reconnectAt = GetTickCount64() + 5000;
            }
        }

        SendPendingActivity();
        Sleep(16);
    }
}
