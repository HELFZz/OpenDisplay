// OpenDisplay shared wire protocol (host.exe <-> Android app).
// One TCP socket (adb reverse). PC->phone: video stream.
// Phone->PC: framed control messages. No interleaving issues:
// directions are independent.
//
// Layout conventions: all multi-byte integers big-endian on the wire.
// Video packet header mirrors scrcpy (Apache-2.0): 12 bytes.
#pragma once
#include <stdint.h>

#define OD_PORT 27190
#define OD_MAGIC 0x4F445350u // "ODSP"
#define OD_PROTO_VERSION 1

// ---- Phone -> PC control framing: [type u8][size u32be][payload] ----
enum OdCtrlType : uint8_t {
    OD_CTRL_CAPS = 1,     // phone capabilities (first message, required)
    OD_CTRL_TOUCH = 2,
    OD_CTRL_KEY = 3,
    OD_CTRL_SCROLL = 4,
    OD_CTRL_EOF = 5,      // phone stops reading video (screen off)
};

// OD_CTRL_CAPS payload (packed, 8 bytes)
// u16 maxW, u16 maxH, u16 maxHz,
// u8 codecFlags (bit0=HEVC, bit1=H264),
// u8 rot (0..3, display rotation of phone, unused v0)
#pragma pack(push, 1)
struct OdCaps {
    uint16_t max_w;
    uint16_t max_h;
    uint16_t max_hz;
    uint8_t codec_flags;
    uint8_t rot;
};

#define OD_CODEC_HEVC 1
#define OD_CODEC_H264 2

// OD_CTRL_TOUCH payload (packed, 18 bytes)
enum OdTouchAction : uint8_t {
    OD_TOUCH_DOWN = 0,
    OD_TOUCH_UP = 1,
    OD_TOUCH_MOVE = 2,
};

struct OdTouch {
    uint8_t action;        // OdTouchAction
    uint8_t buttons;       // bit0=primary (reserved)
    uint32_t pointer_id;   // multi-touch id
    float x;               // 0..1 relative to virtual display width
    float y;               // 0..1 relative to virtual display height
    float pressure;        // 0..1 (unused v0)
};

// OD_CTRL_KEY payload (packed, 10 bytes)
struct OdKey {
    uint8_t action;        // 0=down, 1=up
    uint8_t repeat;        // 1 if auto-repeat
    uint32_t keycode;      // Android KeyEvent keyCode
    uint32_t meta_state;   // Android MetaState
};

// OD_CTRL_SCROLL payload (packed, 20 bytes)
struct OdScroll {
    uint32_t pointer_id;
    float x;
    float y;
    float hscroll;         // horizontal wheel ticks
    float vscroll;         // vertical wheel ticks
};
#pragma pack(pop)

// ---- PC -> phone: handshake (sent right after accept, before video) ----
// [magic u32be][version u16be][codec u8][flags u8]
//   [width u16be][height u16be][hz u16be][reserved u16be]
// codec: OD_CODEC_*. flags bit0=packet headers use scrcpy layout.
#define OD_HS_HEADER_SIZE 16
struct OdHandshake {
    uint32_t magic;
    uint16_t version;
    uint8_t codec;
    uint8_t flags;
    uint16_t width;
    uint16_t height;
    uint16_t hz;
    uint16_t reserved;
};

// ---- PC -> phone: video packets ----
// [pts_and_flags u64be][size u32be][payload]
// pts in low 62 bits (us, unused v0), flags in high 2 bits.
#define OD_VID_HEADER_SIZE 12
#define OD_PACKET_FLAG_CONFIG (1ull << 63)  // SPS/PPS (HEVC: VPS/SPS/PPS)
#define OD_PACKET_FLAG_KEY_FRAME (1ull << 62)
