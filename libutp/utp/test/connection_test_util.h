#ifndef EULAR_UTP_TEST_CONNECTION_TEST_UTIL_H
#define EULAR_UTP_TEST_CONNECTION_TEST_UTIL_H

#include <cassert>

#include "connection/connection.h"

namespace utp_test {

class PacketOutBufferPool
{
public:
    PacketOutBufferPool() { assert(utp_packet_out_buffer_pool_init(&pool_, nullptr) == UTP_INTERNAL_ERROR_OK); }

    ~PacketOutBufferPool() { utp_packet_out_buffer_pool_cleanup(&pool_); }

    utp_packet_out_buffer_pool_t* get() { return &pool_; }

private:
    utp_packet_out_buffer_pool_t pool_ = {};
};

inline utp_packet_out_buffer_pool_t* test_packet_out_buffer_pool()
{
    static PacketOutBufferPool pool;

    return pool.get();
}

inline utp_internal_error_t test_connection_init(utp_connection_t* connection, utp_connection_role_t role,
                                                 uint32_t local_cid, uint32_t peer_cid, const utp_address_t* peer,
                                                 size_t packet_limit, uint16_t packet_capacity)
{
    return utp_connection_init(connection, role, local_cid, peer_cid, peer, packet_limit, packet_capacity,
                               test_packet_out_buffer_pool());
}

}  // namespace utp_test

#define utp_connection_init utp_test::test_connection_init

#endif  // EULAR_UTP_TEST_CONNECTION_TEST_UTIL_H
