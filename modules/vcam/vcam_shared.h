// Shared memory between LunaShare (writer, in the user's session) and the LunaShare Camera media source
// (reader, loaded by the Windows camera Frame Server service). The source creates the mapping, so it exists only
// while some app is using the camera; LunaShare opens it and writes the newest picture.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>

#define LSVC_MAPPING L"Global\\LunaShareVirtualCamera"
#define LSVC_CLSID_STR L"{29D076F7-8632-4FB9-B2C5-9819DA3B835B}"
#define LSVC_NAME L"LunaShare Camera"
#define LSVC_MAGIC 0x4356534Cu  // "LSVC"
#define LSVC_MAX_W 3840
#define LSVC_MAX_H 2160

struct LsvcHeader {
    uint32_t magic;
    uint32_t version;         // 1
    volatile long seq;        // seqlock: odd while the writer is busy
    uint32_t width, height;   // picture in the buffer: NV12, stride = width
    int64_t writtenMs;        // GetTickCount64() of the last write (stale pictures turn into the placeholder)
    uint32_t wantW, wantH;    // size the camera user picked (the writer may scale down to it)
    uint32_t reserved[16];
};

#define LSVC_DATA_OFFSET 256
#define LSVC_DATA_SIZE ((size_t)LSVC_MAX_W * LSVC_MAX_H * 3 / 2)
#define LSVC_MAPPING_SIZE (LSVC_DATA_OFFSET + LSVC_DATA_SIZE)
