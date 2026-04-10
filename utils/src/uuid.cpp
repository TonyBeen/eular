/*************************************************************************
    > File Name: uuid.cpp
    > Author: eular
    > Brief:
    > Created Time: Fri 10 Apr 2026 10:58:21 AM CST
 ************************************************************************/

#include "utils/uuid.h"

#include <string.h>

#include "src/sha1.h"
#include "src/printf.h"

namespace eular {
const uuid_t UUID::UUID_NS_DNS = {{0x6b,0xa7,0xb8,0x10,0x9d,0xad,0x11,0xd1,0x80,0xb4,0x00,0xc0,0x4f,0xd4,0x30,0xc8}};
const uuid_t UUID::UUID_NS_URL = {{0x6b,0xa7,0xb8,0x11,0x9d,0xad,0x11,0xd1,0x80,0xb4,0x00,0xc0,0x4f,0xd4,0x30,0xc8}};

uuid_t UUID::V5(const uuid_t &ns, const std::string &name)
{
    uuid_t out;
    SHA1_CTX sha1Ctx;
    SHA1_Init(&sha1Ctx);
    SHA1_Update(&sha1Ctx, ns.data(), ns.size());
    SHA1_Update(&sha1Ctx, reinterpret_cast<const uint8_t*>(name.data()), name.size());

    uint8_t hash[20];
    SHA1_Final(hash, &sha1Ctx);
    memcpy(out.data(), hash, 16);
    // 设置版本号 (0101 -> 5)
    out[6] = (out[6] & 0x0F) | 0x50;
    // 设置变体 (10xx -> RFC 4122)
    out[8] = (out[8] & 0x3F) | 0x80;
    return out;
}

std::string UUID::ToString(const uuid_t &uuid)
{
    char buf[64];
    snprintf_(buf, sizeof(buf),
              "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
              uuid[0], uuid[1], uuid[2], uuid[3],
              uuid[4], uuid[5],
              uuid[6], uuid[7],
              uuid[8], uuid[9],
              uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
    return std::string(buf);
}
} // namespace eular
