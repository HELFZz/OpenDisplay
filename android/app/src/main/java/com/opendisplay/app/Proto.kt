package com.opendisplay.app

// Wire protocol mirror of proto/opendisplay_proto.h (big-endian on the wire).

object Proto {
    const val PORT = 27183
    const val MAGIC = 0x4F445350 // "ODSP"
    const val VERSION = 1

    // Control types (phone -> PC)
    const val CTRL_CAPS = 1
    const val CTRL_TOUCH = 2
    const val CTRL_KEY = 3
    const val CTRL_SCROLL = 4
    const val CTRL_EOF = 5

    // Codec flags
    const val CODEC_HEVC = 1
    const val CODEC_H264 = 2

    // Touch actions
    const val TOUCH_DOWN = 0
    const val TOUCH_UP = 1
    const val TOUCH_MOVE = 2

    // Video packet flags (in pts_and_flags high bits)
    const val PACKET_FLAG_CONFIG = 1L shl 63
    const val PACKET_FLAG_KEY_FRAME = 1L shl 62

    const val HS_HEADER_SIZE = 16
    const val VID_HEADER_SIZE = 12
}
