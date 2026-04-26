/*************************************************************************
    > File Name: packet_out.cpp
    > Author: eular
    > Brief:
    > Created Time: Wed 21 Jan 2026 03:09:11 PM CST
 ************************************************************************/

#include "proto/packet_out.h"

#include <cstdlib>
#include <cstring>
#include <limits>

namespace eular {
namespace utp {

bool PacketOut::addSendAttempt(utp_packno_t packetNo, utp_time_t sentTime)
{
    PacketOutAttempt *attempt = new (std::nothrow) PacketOutAttempt();
    if (attempt == nullptr) {
        return false;
    }

    attempt->packet_no = packetNo;
    attempt->sent_time = sentTime;
    attempt->next = nullptr;

    if (attempts_tail != nullptr) {
        attempts_tail->next = attempt;
    } else {
        attempts_head = attempt;
    }
    attempts_tail = attempt;
    if (attempts_count < (std::numeric_limits<uint16_t>::max)()) {
        ++attempts_count;
    }

    return true;
}

void PacketOut::clearSendAttempts()
{
    PacketOutAttempt *attempt = attempts_head;
    while (attempt != nullptr) {
        PacketOutAttempt *next = attempt->next;
        delete attempt;
        attempt = next;
    }

    attempts_head = nullptr;
    attempts_tail = nullptr;
    attempts_count = 0;
}

void PacketOut::reset()
{
    clearSendAttempts();

    if (encrypt_data != nullptr && encrypt_data != raw_data) {
        std::free(encrypt_data);
    }

    uint8_t *raw = raw_data;
    uint16_t alloc = alloc_size;

    std::memset(this, 0, sizeof(PacketOut));

    raw_data = raw;
    alloc_size = alloc;
    loss_chain = this;
}

} // namespace utp
} // namespace eular