/*************************************************************************
    > File Name: util.cpp
    > Author: eular
    > Brief:
    > Created Time: Wed 14 Jan 2026 09:42:12 PM CST
 ************************************************************************/

#include "util/util.h"

#include <random>
#include <siphash.h>

void eular::utp::Util::RandomBytes(void* buf, size_t len)
{
    std::random_device rd;  // Obtain a random number from hardware
    std::mt19937 eng(rd()); // Seed the generator
    std::uniform_int_distribution<> distr(0, 255);

    uint8_t *byteBuf = static_cast<uint8_t*>(buf);
    for (size_t i = 0; i < len; ++i) {
        byteBuf[i] = distr(eng);
    }
}

uint32_t eular::utp::Util::GenerateLocalConnectionId(const void *key, const char *localHost, uint16_t localPort, utp_time_t timestamp, uint8_t attempt)
{
    char buffer[256];
    int32_t size = snprintf(buffer, sizeof(buffer), "L#%s#%u#%u#%" PRIu64, localHost, localPort, attempt, timestamp);
    if (size < 0) {
        return 0;
    }

    uint64_t hash = siphash(buffer, size, key);
    return static_cast<uint32_t>(hash ^ (hash >> 32));
}

uint32_t eular::utp::Util::GenerateRemoteConnectionId(const void *key, const char *peerIp, uint32_t peerCid, utp_time_t timestamp, uint8_t attempt)
{
    char buffer[256];
    int32_t size = snprintf(buffer, sizeof(buffer), "R#%s#%u#%u#%" PRIu64, peerIp, peerCid, attempt, timestamp);
    if (size < 0) {
        return 0;
    }

    uint64_t hash = siphash(buffer, size, key);
    return static_cast<uint32_t>(hash ^ (hash >> 32));
}