/*************************************************************************
    > File Name: packet_out.cpp
    > Author: eular
    > Brief:
    > Created Time: Wed 21 Jan 2026 03:09:11 PM CST
 ************************************************************************/

#include "proto/packet_out.h"

#include <limits>

#include "crypto/aes_gcm_context.h"
#include "util/fiu_local.h"

namespace eular {
namespace utp {

bool PacketOut::addSendAttempt(utp_packno_t packetNo, utp_time_t sentTime)
{
    (void) packetNo;
    (void) sentTime;
#if defined(UTP_ENABLE_FAULT_INJECTION)
    if (fiu_fail("mem/packet_out_attempt/alloc")) {
        return false;
    }
#endif
    if (attempts_count < (std::numeric_limits<uint16_t>::max)()) {
        ++attempts_count;
    }

    return true;
}

void PacketOut::clearSendAttempts()
{
    attempts_head = nullptr;
    attempts_tail = nullptr;
    attempts_count = 0;
}

void PacketOut::initForReuse(uint8_t *raw, uint16_t alloc)
{
    raw_data = raw;
    alloc_size = alloc;
    sent_time = 0;
    packno = 0;
    ackno = 0;
    loss_chain = this;
    frame_types = 0;
    po_flags = 0;
    local_flags = 0;
    data_size = 0;
    encrypt_data_size = 0;
    slice_count = 0;
    frame_meta_count = 0;
    stream_data_size = 0;
    transient_ack_size = 0;
    stream_id = 0;
    stream_offset = 0;
    encrypt_data = nullptr;
    bw_state = nullptr;
    attempts_head = nullptr;
    attempts_tail = nullptr;
    attempts_count = 0;
}

void PacketOut::reset()
{
    clearSendAttempts();

    if (encrypt_data != nullptr && encrypt_data != raw_data) {
        AesGcmContext::ReleaseEncryptBuffer(encrypt_data, encrypt_data_size);
    }

    initForReuse(raw_data, alloc_size);
}

} // namespace utp
} // namespace eular
