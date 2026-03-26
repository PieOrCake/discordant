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

private:
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
    std::vector<uint8_t> m_recvBuf;
    std::string m_fragmentBuf; // for fragmented messages
};

#endif // DISCORDANT_WEBSOCKET_H
