#include "server.h"

#include <ws2tcpip.h>

#include <cstring>

namespace {
constexpr size_t kMaxControlPayload = 4096;
constexpr size_t kMaxVideoPacket = 64u * 1024 * 1024;
}

Server::~Server() {
    closeClient();
    if (m_listen != INVALID_SOCKET)
        closesocket(m_listen);
}

bool Server::listenOn(uint16_t port, std::string& err) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        err = "WSAStartup failed";
        return false;
    }
    m_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listen == INVALID_SOCKET) {
        err = "socket() failed";
        return false;
    }
    BOOL yes = TRUE;
    setsockopt(m_listen, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // adb reverse targets localhost
    if (bind(m_listen, (const sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        err = "bind() failed (port in use?)";
        return false;
    }
    if (listen(m_listen, 1) == SOCKET_ERROR) {
        err = "listen() failed";
        return false;
    }
    return true;
}

bool Server::acceptClient(std::string& err) {
    closeClient();
    sockaddr_in ca = {};
    int calen = sizeof(ca);
    m_client = accept(m_listen, (sockaddr*)&ca, &calen);
    if (m_client == INVALID_SOCKET) {
        err = "accept() failed";
        return false;
    }
    BOOL yes = TRUE;
    setsockopt(m_client, IPPROTO_TCP, TCP_NODELAY, (const char*)&yes, sizeof(yes));
    return true;
}

void Server::closeClient() {
    if (m_client != INVALID_SOCKET) {
        closesocket(m_client);
        m_client = INVALID_SOCKET;
    }
}

bool Server::recvExact(void* buf, size_t len, std::string& err) {
    char* p = static_cast<char*>(buf);
    while (len > 0) {
        int n = recv(m_client, p, (int)len, 0);
        if (n <= 0) {
            err = "recv() failed or closed";
            return false;
        }
        p += n;
        len -= n;
    }
    return true;
}

bool Server::sendExact(const void* buf, size_t len, std::string& err) {
    const char* p = static_cast<const char*>(buf);
    while (len > 0) {
        int n = send(m_client, p, (int)len, 0);
        if (n <= 0) {
            err = "send() failed";
            return false;
        }
        p += n;
        len -= n;
    }
    return true;
}

bool Server::readCaps(OdCaps& caps, std::string& err) {
    uint8_t hdr[5];
    if (!recvExact(hdr, sizeof(hdr), err))
        return false;
    if (hdr[0] != OD_CTRL_CAPS) {
        err = "expected OD_CTRL_CAPS";
        return false;
    }
    uint32_t size = be32toh(*(uint32_t*)(hdr + 1));
    if (size != sizeof(OdCaps)) {
        err = "bad caps size";
        return false;
    }
    uint8_t payload[sizeof(OdCaps)];
    if (!recvExact(payload, sizeof(payload), err))
        return false;
    memcpy(&caps, payload, sizeof(caps));
    caps.max_w = be16toh(caps.max_w);
    caps.max_h = be16toh(caps.max_h);
    caps.max_hz = be16toh(caps.max_hz);
    return true;
}

bool Server::sendHandshake(uint8_t codec, uint16_t w, uint16_t h, uint16_t hz, std::string& err) {
    uint8_t buf[OD_HS_HEADER_SIZE];
    uint32_t magic = htobe32(OD_MAGIC);
    memcpy(buf + 0, &magic, 4);
    *(uint16_t*)(buf + 4) = htobe16(OD_PROTO_VERSION);
    buf[6] = codec;
    buf[7] = 0; // flags
    *(uint16_t*)(buf + 8) = htobe16(w);
    *(uint16_t*)(buf + 10) = htobe16(h);
    *(uint16_t*)(buf + 12) = htobe16(hz);
    *(uint16_t*)(buf + 14) = 0;
    return sendExact(buf, sizeof(buf), err);
}

bool Server::sendVideo(const uint8_t* payload, uint32_t size, uint64_t ptsFlags, std::string& err) {
    uint8_t hdr[OD_VID_HEADER_SIZE];
    uint64_t pf = htobe64(ptsFlags);
    memcpy(hdr, &pf, 8);
    *(uint32_t*)(hdr + 8) = htobe32(size);
    if (!sendExact(hdr, sizeof(hdr), err))
        return false;
    return sendExact(payload, size, err);
}

bool Server::pumpControl(const TouchHandler& onTouch, const KeyHandler& onKey,
                         const ScrollHandler& onScroll, const EofHandler& onEof,
                         std::string& err) {
    for (;;) {
        uint8_t hdr[5];
        if (!recvExact(hdr, sizeof(hdr), err))
            return false;
        const uint8_t type = hdr[0];
        const uint32_t size = be32toh(*(uint32_t*)(hdr + 1));
        if (size > kMaxControlPayload) {
            err = "control payload too large";
            return false;
        }
        uint8_t payload[kMaxControlPayload];
        if (size > 0 && !recvExact(payload, size, err))
            return false;

        switch (type) {
        case OD_CTRL_TOUCH:
            if (size >= sizeof(OdTouch) && onTouch) {
                OdTouch t;
                t.action = payload[0];
                t.buttons = payload[1];
                memcpy(&t.pointer_id, payload + 2, 4);
                memcpy(&t.x, payload + 6, 4);
                memcpy(&t.y, payload + 10, 4);
                memcpy(&t.pressure, payload + 14, 4);
                // Wire layout must match struct packing; phone sends packed layout.
                onTouch(t);
            }
            break;
        case OD_CTRL_KEY:
            if (size >= sizeof(OdKey) && onKey) {
                OdKey k;
                k.action = payload[0];
                k.repeat = payload[1];
                memcpy(&k.keycode, payload + 2, 4);
                memcpy(&k.meta_state, payload + 6, 4);
                onKey(k);
            }
            break;
        case OD_CTRL_SCROLL:
            if (size >= sizeof(OdScroll) && onScroll) {
                OdScroll s;
                memcpy(&s.pointer_id, payload + 0, 4);
                memcpy(&s.x, payload + 4, 4);
                memcpy(&s.y, payload + 8, 4);
                memcpy(&s.hscroll, payload + 12, 4);
                memcpy(&s.vscroll, payload + 16, 4);
                onScroll(s);
            }
            break;
        case OD_CTRL_EOF:
            if (onEof)
                onEof();
            return true;
        default:
            break; // ignore unknown
        }
    }
}
