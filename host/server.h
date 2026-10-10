#pragma once
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../proto/opendisplay_proto.h"

// TCP server for one phone connection. Wire helpers use big-endian.
class Server {
public:
    using TouchHandler = std::function<void(const OdTouch&)>;
    using KeyHandler = std::function<void(const OdKey&)>;
    using ScrollHandler = std::function<void(const OdScroll&)>;
    using EofHandler = std::function<void()>;

    ~Server();

    bool listenOn(uint16_t port, std::string& err);
    // Blocking accept; fills err on failure. Returns false if error.
    bool acceptClient(std::string& err);
    // Blocking read of OD_CTRL_CAPS (first message). Returns false on error.
    bool readCaps(OdCaps& caps, std::string& err);
    // Send 16-byte handshake (big-endian fields).
    bool sendHandshake(uint8_t codec, uint16_t w, uint16_t h, uint16_t hz, std::string& err);
    // Send one video packet: [pts_flags u64be][size u32be][payload].
    bool sendVideo(const uint8_t* payload, uint32_t size, uint64_t ptsFlags, std::string& err);

    // Blocking control-message pump; dispatches handlers until EOF/error.
    // Returns false when the connection is closed or on error.
    bool pumpControl(const TouchHandler& onTouch, const KeyHandler& onKey,
                     const ScrollHandler& onScroll, const EofHandler& onEof,
                     std::string& err);

    void closeClient();

private:
    bool recvExact(void* buf, size_t len, std::string& err);
    bool sendExact(const void* buf, size_t len, std::string& err);

    SOCKET m_listen = INVALID_SOCKET;
    SOCKET m_client = INVALID_SOCKET;
};

// Big-endian helpers.
inline uint16_t od_bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
inline uint32_t od_bswap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24);
}
inline uint64_t od_bswap64(uint64_t v) {
    return ((uint64_t)od_bswap32((uint32_t)v) << 32) | od_bswap32((uint32_t)(v >> 32));
}

#if defined(_WIN32)
#define OD_LITTLE_ENDIAN 1
#define htobe16 od_bswap16
#define htobe32 od_bswap32
#define htobe64 od_bswap64
#define be16toh od_bswap16
#define be32toh od_bswap32
#define be64toh od_bswap64
#endif
