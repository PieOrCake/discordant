#ifndef DISCORDANT_WEBSOCKET_H
#define DISCORDANT_WEBSOCKET_H

#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

// Minimal WebSocket client for localhost connections (no TLS).
// Implements just enough of RFC 6455 to talk to Discord's local RPC server.
class WebSocket {
public:
    enum class State {
        Disconnected,
        Connecting,
        Connected,
        Closing
    };

    WebSocket();
    ~WebSocket();

    // Connect to ws://host:port with the given path. Returns true on success.
    bool Connect(const std::string& host, int port, const std::string& path,
                 const std::string& origin = "");

    // Send a text frame.
    bool SendText(const std::string& message);

    // Poll for incoming messages. Non-blocking. Returns received text frames.
    std::vector<std::string> Poll();

    // Close the connection gracefully.
    void Close();

    State GetState() const { return m_state; }

    // Human-readable reason the last Connect() failed. Empty on success.
    const std::string& GetLastError() const { return m_lastError; }

    // True if the last Connect() failure was a plain TCP refusal/timeout, i.e.
    // nothing was listening on that port. Distinguishes "Discord isn't there"
    // from "Discord answered but rejected us".
    bool LastErrorWasRefused() const { return m_lastErrorRefused; }

private:
    // Record a Connect() failure reason and return false, for terse call sites.
    bool Fail(const std::string& reason, bool refused);

    // Generate a random 16-byte masking key (base64-encoded for Sec-WebSocket-Key).
    static std::string GenerateKey();

    // Send raw bytes.
    bool SendRaw(const char* data, int len);

    // Receive into internal buffer. Returns false on error/disconnect.
    bool ReceiveIntoBuffer();

    // Parse WebSocket frames from the buffer. Returns decoded text payloads.
    std::vector<std::string> ParseFrames();

    // Build and send a WebSocket frame.
    bool SendFrame(uint8_t opcode, const uint8_t* payload, size_t len);

    SOCKET m_socket;
    State m_state;
    std::string m_lastError;
    bool m_lastErrorRefused;
    std::vector<uint8_t> m_recvBuf;
    std::string m_fragmentBuf; // for fragmented messages
};

#endif // DISCORDANT_WEBSOCKET_H
