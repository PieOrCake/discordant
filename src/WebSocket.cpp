#include "WebSocket.h"
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <sstream>

// Base64 encode for WebSocket key
static const char B64_TABLE[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string Base64Encode(const unsigned char* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned int n = ((unsigned int)data[i]) << 16;
        if (i + 1 < len) n |= ((unsigned int)data[i + 1]) << 8;
        if (i + 2 < len) n |= (unsigned int)data[i + 2];
        out += B64_TABLE[(n >> 18) & 0x3F];
        out += B64_TABLE[(n >> 12) & 0x3F];
        out += (i + 1 < len) ? B64_TABLE[(n >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? B64_TABLE[n & 0x3F] : '=';
    }
    return out;
}

static void EnsureWSAStartup() {
    static bool wsaInited = false;
    if (!wsaInited) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        wsaInited = true;
    }
}

WebSocket::WebSocket()
    : m_socket(INVALID_SOCKET)
    , m_state(State::Disconnected)
{
    // WSAStartup deferred to Connect() to avoid calling it during DllMain
}

WebSocket::~WebSocket() {
    Close();
}

std::string WebSocket::GenerateKey() {
    unsigned char bytes[16];
    // Simple PRNG seeded by time — sufficient for a WebSocket handshake key
    static bool seeded = false;
    if (!seeded) { srand((unsigned int)time(nullptr)); seeded = true; }
    for (int i = 0; i < 16; i++) bytes[i] = (unsigned char)(rand() & 0xFF);
    return Base64Encode(bytes, 16);
}

bool WebSocket::Connect(const std::string& host, int port, const std::string& path,
                        const std::string& origin) {
    EnsureWSAStartup();
    Close();

    m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_socket == INVALID_SOCKET) return false;

    // Set non-blocking BEFORE connect to avoid blocking the render thread
    u_long nbMode = 1;
    ioctlsocket(m_socket, FIONBIO, &nbMode);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = inet_addr(host.c_str());

    int ret = connect(m_socket, (struct sockaddr*)&addr, sizeof(addr));
    if (ret == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            closesocket(m_socket);
            m_socket = INVALID_SOCKET;
            return false;
        }
        // Connection in progress — use select() with a short timeout
        fd_set writefds, exceptfds;
        FD_ZERO(&writefds);
        FD_ZERO(&exceptfds);
        FD_SET(m_socket, &writefds);
        FD_SET(m_socket, &exceptfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50000; // 50ms timeout

        ret = select(0, NULL, &writefds, &exceptfds, &tv);
        if (ret <= 0 || FD_ISSET(m_socket, &exceptfds)) {
            closesocket(m_socket);
            m_socket = INVALID_SOCKET;
            return false;
        }

        // Verify connection actually succeeded
        int optval = 0;
        int optlen = sizeof(optval);
        getsockopt(m_socket, SOL_SOCKET, SO_ERROR, (char*)&optval, &optlen);
        if (optval != 0) {
            closesocket(m_socket);
            m_socket = INVALID_SOCKET;
            return false;
        }
    }

    // TCP connected — switch to blocking briefly for the HTTP handshake
    // (localhost handshake is fast; set a recv timeout as safety net)
    nbMode = 0;
    ioctlsocket(m_socket, FIONBIO, &nbMode);
    DWORD timeout = 500; // 500ms max for handshake
    setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(m_socket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

    m_state = State::Connecting;

    // WebSocket upgrade handshake
    std::string key = GenerateKey();
    std::ostringstream req;
    req << "GET " << path << " HTTP/1.1\r\n";
    req << "Host: " << host << ":" << port << "\r\n";
    req << "Upgrade: websocket\r\n";
    req << "Connection: Upgrade\r\n";
    req << "Sec-WebSocket-Key: " << key << "\r\n";
    req << "Sec-WebSocket-Version: 13\r\n";
    if (!origin.empty()) {
        req << "Origin: " << origin << "\r\n";
    }
    req << "\r\n";

    std::string reqStr = req.str();
    if (!SendRaw(reqStr.c_str(), (int)reqStr.size())) {
        Close();
        return false;
    }

    // Read HTTP response
    char buf[4096];
    int total = 0;
    bool headersDone = false;
    while (!headersDone && total < (int)sizeof(buf) - 1) {
        int n = recv(m_socket, buf + total, (int)sizeof(buf) - 1 - total, 0);
        if (n <= 0) { Close(); return false; }
        total += n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) headersDone = true;
    }

    // Verify we got a 101 Switching Protocols
    if (!strstr(buf, "101")) {
        Close();
        return false;
    }

    // Any data after the headers is the start of WebSocket frames
    char* bodyStart = strstr(buf, "\r\n\r\n");
    if (bodyStart) {
        bodyStart += 4;
        int remaining = total - (int)(bodyStart - buf);
        if (remaining > 0) {
            m_recvBuf.insert(m_recvBuf.end(), (uint8_t*)bodyStart, (uint8_t*)bodyStart + remaining);
        }
    }

    // Set non-blocking for Poll()
    nbMode = 1;
    ioctlsocket(m_socket, FIONBIO, &nbMode);

    m_state = State::Connected;
    return true;
}

bool WebSocket::SendRaw(const char* data, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(m_socket, data + sent, len - sent, 0);
        if (n == SOCKET_ERROR) return false;
        sent += n;
    }
    return true;
}

bool WebSocket::SendFrame(uint8_t opcode, const uint8_t* payload, size_t len) {
    std::vector<uint8_t> frame;

    // FIN + opcode
    frame.push_back(0x80 | opcode);

    // Mask bit set (client must mask), payload length
    if (len < 126) {
        frame.push_back(0x80 | (uint8_t)len);
    } else if (len <= 0xFFFF) {
        frame.push_back(0x80 | 126);
        frame.push_back((uint8_t)((len >> 8) & 0xFF));
        frame.push_back((uint8_t)(len & 0xFF));
    } else {
        frame.push_back(0x80 | 127);
        for (int i = 7; i >= 0; i--) {
            frame.push_back((uint8_t)((len >> (8 * i)) & 0xFF));
        }
    }

    // Masking key
    uint8_t mask[4];
    for (int i = 0; i < 4; i++) mask[i] = (uint8_t)(rand() & 0xFF);
    frame.insert(frame.end(), mask, mask + 4);

    // Masked payload
    for (size_t i = 0; i < len; i++) {
        frame.push_back(payload[i] ^ mask[i % 4]);
    }

    return SendRaw((const char*)frame.data(), (int)frame.size());
}

bool WebSocket::SendText(const std::string& message) {
    if (m_state != State::Connected) return false;
    return SendFrame(0x01, (const uint8_t*)message.data(), message.size());
}

bool WebSocket::ReceiveIntoBuffer() {
    uint8_t buf[8192];
    int n = recv(m_socket, (char*)buf, sizeof(buf), 0);
    if (n > 0) {
        m_recvBuf.insert(m_recvBuf.end(), buf, buf + n);
        return true;
    }
    if (n == 0) {
        // Connection closed
        m_state = State::Disconnected;
        return false;
    }
    // WSAEWOULDBLOCK is expected for non-blocking
    int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK) return true;
    m_state = State::Disconnected;
    return false;
}

std::vector<std::string> WebSocket::ParseFrames() {
    std::vector<std::string> messages;

    while (m_recvBuf.size() >= 2) {
        size_t pos = 0;
        uint8_t b0 = m_recvBuf[pos++];
        uint8_t b1 = m_recvBuf[pos++];

        bool fin = (b0 & 0x80) != 0;
        uint8_t opcode = b0 & 0x0F;
        bool masked = (b1 & 0x80) != 0;
        uint64_t payloadLen = b1 & 0x7F;

        if (payloadLen == 126) {
            if (m_recvBuf.size() < pos + 2) break; // need more data
            payloadLen = ((uint64_t)m_recvBuf[pos] << 8) | m_recvBuf[pos + 1];
            pos += 2;
        } else if (payloadLen == 127) {
            if (m_recvBuf.size() < pos + 8) break;
            payloadLen = 0;
            for (int i = 0; i < 8; i++) {
                payloadLen = (payloadLen << 8) | m_recvBuf[pos + i];
            }
            pos += 8;
        }

        uint8_t mask[4] = {0};
        if (masked) {
            if (m_recvBuf.size() < pos + 4) break;
            memcpy(mask, &m_recvBuf[pos], 4);
            pos += 4;
        }

        if (m_recvBuf.size() < pos + payloadLen) break; // need more data

        // Extract payload
        std::string payload;
        payload.resize((size_t)payloadLen);
        for (uint64_t i = 0; i < payloadLen; i++) {
            payload[i] = (char)(m_recvBuf[pos + i] ^ (masked ? mask[i % 4] : 0));
        }
        pos += (size_t)payloadLen;

        // Remove consumed bytes
        m_recvBuf.erase(m_recvBuf.begin(), m_recvBuf.begin() + pos);

        if (opcode == 0x01 || opcode == 0x02) {
            // Text or binary frame
            if (fin) {
                if (!m_fragmentBuf.empty()) {
                    m_fragmentBuf += payload;
                    messages.push_back(std::move(m_fragmentBuf));
                    m_fragmentBuf.clear();
                } else {
                    messages.push_back(std::move(payload));
                }
            } else {
                m_fragmentBuf += payload;
            }
        } else if (opcode == 0x00) {
            // Continuation
            m_fragmentBuf += payload;
            if (fin) {
                messages.push_back(std::move(m_fragmentBuf));
                m_fragmentBuf.clear();
            }
        } else if (opcode == 0x08) {
            // Close
            SendFrame(0x08, nullptr, 0);
            m_state = State::Closing;
        } else if (opcode == 0x09) {
            // Ping — respond with pong
            SendFrame(0x0A, (const uint8_t*)payload.data(), payload.size());
        }
        // 0x0A = pong, ignore
    }

    return messages;
}

std::vector<std::string> WebSocket::Poll() {
    if (m_state != State::Connected) return {};
    ReceiveIntoBuffer();
    return ParseFrames();
}

void WebSocket::Close() {
    if (m_socket != INVALID_SOCKET) {
        if (m_state == State::Connected) {
            SendFrame(0x08, nullptr, 0);
        }
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
    m_state = State::Disconnected;
    m_recvBuf.clear();
    m_fragmentBuf.clear();
}
