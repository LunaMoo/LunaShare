#include "protocol.h"

#include <cstring>

namespace ls {

bool WriteFrame(SocketHandle s, Msg type, const void* payload, size_t len) {
    if (len + 1 > kMaxFrame) return false;
    uint8_t hdr[5];
    PutU32(hdr, (uint32_t)(len + 1));
    hdr[4] = (uint8_t)type;
    if (len < 4096) {  // coalesce small frames into one send
        std::vector<uint8_t> b(5 + len);
        memcpy(b.data(), hdr, 5);
        if (len) memcpy(b.data() + 5, payload, len);
        return SendAll(s, b.data(), b.size());
    }
    return SendAll(s, hdr, 5) && SendAll(s, payload, len);
}

bool WriteFrame(SocketHandle s, Msg type, const std::string& json) {
    return WriteFrame(s, type, json.data(), json.size());
}

bool ReadFrame(SocketHandle s, Frame& out) {
    uint8_t hdr[5];
    if (!RecvAll(s, hdr, 5)) return false;
    uint32_t len = GetU32(hdr);
    if (len == 0 || len > kMaxFrame) return false;
    out.type = (Msg)hdr[4];
    out.payload.resize(len - 1);
    if (len > 1 && !RecvAll(s, out.payload.data(), len - 1)) return false;
    return true;
}

}  // namespace ls
