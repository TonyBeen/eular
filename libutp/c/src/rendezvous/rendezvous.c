#include "rendezvous/rendezvous.h"

#include <string.h>

#include "proto/wire.h"

static size_t utp_rendezvous_address_length(uint8_t family)
{
    return family == UTP_ADDRESS_FAMILY_IPV4 ? 4u : family == UTP_ADDRESS_FAMILY_IPV6 ? 16u : 0u;
}

static utp_internal_error_t utp_rendezvous_write_bytes(utp_wire_writer_t* writer, const uint8_t* data, size_t length)
{
    utp_internal_error_t error = UTP_INTERNAL_ERROR_OK;
    size_t               index;

    if (data == NULL && length != 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    for (index = 0u; index < length && error == UTP_INTERNAL_ERROR_OK; ++index) {
        error = utp_wire_write_u8(writer, data[index]);
    }
    return error;
}

static utp_internal_error_t utp_rendezvous_read_bytes(utp_wire_reader_t* reader, uint8_t* data, size_t length)
{
    utp_internal_error_t error = UTP_INTERNAL_ERROR_OK;
    size_t               index;

    if (data == NULL && length != 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    for (index = 0u; index < length && error == UTP_INTERNAL_ERROR_OK; ++index) {
        error = utp_wire_read_u8(reader, &data[index]);
    }
    return error;
}

static utp_internal_error_t utp_rendezvous_validate_register(const utp_rendezvous_register_t* registration)
{
    size_t address_length;
    size_t index;

    if (registration == NULL || registration->peer_id == NULL || registration->peer_id_length == 0u ||
        registration->peer_id_length > UTP_PEER_ID_MAX_LENGTH || registration->local_port == 0u ||
        registration->local_candidate_count > UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES ||
        (registration->local_candidate_count != 0u && registration->local_candidates == NULL)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    address_length = utp_rendezvous_address_length(registration->local_family);
    if (address_length == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    for (index = 0u; index < registration->local_candidate_count; ++index) {
        if (registration->local_candidates[index].family != registration->local_family ||
            registration->local_candidates[index].port != registration->local_port) {
            return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
        }
    }
    return UTP_INTERNAL_ERROR_OK;
}

static utp_internal_error_t utp_rendezvous_validate_registered(const utp_rendezvous_registered_t* registered)
{
    size_t index;

    if (registered == NULL || registered->calibration_endpoint_count > UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES ||
        (registered->calibration_endpoint_count != 0u && registered->calibration_endpoints == NULL) ||
        (registered->calibration_id == 0u && registered->calibration_endpoint_count != 0u) ||
        (registered->calibration_id != 0u && registered->calibration_endpoint_count == 0u)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    for (index = 0u; index < registered->calibration_endpoint_count; ++index) {
        if (registered->calibration_endpoints[index].port == 0u ||
            utp_rendezvous_address_length(registered->calibration_endpoints[index].family) == 0u) {
            return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
        }
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_rendezvous_register_encode(uint8_t* buffer, size_t capacity,
                                                    const utp_rendezvous_register_t* registration, size_t* out_length)
{
    utp_wire_writer_t    writer;
    utp_internal_error_t error;
    size_t               address_length;
    size_t               index;

    if (buffer == NULL || out_length == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_rendezvous_validate_register(registration);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    address_length = utp_rendezvous_address_length(registration->local_family);
    error          = utp_wire_writer_init(&writer, buffer, capacity);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u64(&writer, registration->registration_request_id);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_rendezvous_write_bytes(&writer, registration->registration_token,
                                           sizeof(registration->registration_token));
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u8(&writer, registration->peer_id_length);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_rendezvous_write_bytes(&writer, registration->peer_id, registration->peer_id_length);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u8(&writer, registration->nat_class);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u8(&writer, registration->local_family);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u16(&writer, registration->local_port);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u8(&writer, registration->local_candidate_count);
    }
    for (index = 0u; index < registration->local_candidate_count && error == UTP_INTERNAL_ERROR_OK; ++index) {
        error = utp_rendezvous_write_bytes(&writer, registration->local_candidates[index].address, address_length);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        *out_length = capacity - writer.remaining;
    }
    return error;
}

utp_internal_error_t utp_rendezvous_register_decode(utp_rendezvous_register_t* registration, const uint8_t* buffer,
                                                    size_t length)
{
    utp_wire_reader_t         reader;
    utp_rendezvous_register_t decoded = {0};
    utp_internal_error_t      error;
    size_t                    address_length;
    size_t                    index;

    if (registration == NULL || buffer == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_wire_reader_init(&reader, buffer, length);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u64(&reader, &decoded.registration_request_id);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_rendezvous_read_bytes(&reader, decoded.registration_token, sizeof(decoded.registration_token));
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u8(&reader, &decoded.peer_id_length);
    }
    if (error != UTP_INTERNAL_ERROR_OK || decoded.peer_id_length == 0u ||
        decoded.peer_id_length > UTP_PEER_ID_MAX_LENGTH || reader.remaining < decoded.peer_id_length) {
        return error == UTP_INTERNAL_ERROR_OK ? UTP_INTERNAL_ERROR_PROTOCOL : error;
    }
    decoded.peer_id   = reader.cursor;
    reader.cursor    += decoded.peer_id_length;
    reader.remaining -= decoded.peer_id_length;
    if (utp_wire_read_u8(&reader, &decoded.nat_class) != UTP_INTERNAL_ERROR_OK ||
        utp_wire_read_u8(&reader, &decoded.local_family) != UTP_INTERNAL_ERROR_OK ||
        utp_wire_read_u16(&reader, &decoded.local_port) != UTP_INTERNAL_ERROR_OK ||
        utp_wire_read_u8(&reader, &decoded.local_candidate_count) != UTP_INTERNAL_ERROR_OK) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    address_length = utp_rendezvous_address_length(decoded.local_family);
    if (decoded.local_port == 0u || decoded.local_candidate_count > UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES ||
        address_length == 0u || reader.remaining != address_length * (size_t)decoded.local_candidate_count) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    for (index = 0u; index < decoded.local_candidate_count; ++index) {
        decoded.decoded_local_candidates[index].family = decoded.local_family;
        decoded.decoded_local_candidates[index].port   = decoded.local_port;
        error = utp_rendezvous_read_bytes(&reader, decoded.decoded_local_candidates[index].address, address_length);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
    }
    decoded.local_candidates       = decoded.decoded_local_candidates;
    *registration                  = decoded;
    registration->local_candidates = registration->decoded_local_candidates;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_rendezvous_registered_encode(uint8_t* buffer, size_t capacity,
                                                      const utp_rendezvous_registered_t* registered, size_t* out_length)
{
    utp_wire_writer_t    writer;
    utp_internal_error_t error;
    size_t               index;
    size_t               address_length;

    if (buffer == NULL || out_length == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_rendezvous_validate_registered(registered);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    error = utp_wire_writer_init(&writer, buffer, capacity);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u64(&writer, registered->registration_request_id);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error =
            utp_rendezvous_write_bytes(&writer, registered->registration_token, sizeof(registered->registration_token));
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u64(&writer, registered->calibration_id);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u8(&writer, registered->calibration_endpoint_count);
    }
    for (index = 0u; index < registered->calibration_endpoint_count && error == UTP_INTERNAL_ERROR_OK; ++index) {
        const utp_address_t* const endpoint = &registered->calibration_endpoints[index];

        address_length = utp_rendezvous_address_length(endpoint->family);
        error          = utp_wire_write_u8(&writer, endpoint->family);
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_wire_write_u16(&writer, endpoint->port);
        }
        if (error == UTP_INTERNAL_ERROR_OK) {
            error = utp_rendezvous_write_bytes(&writer, endpoint->address, address_length);
        }
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        *out_length = capacity - writer.remaining;
    }
    return error;
}

utp_internal_error_t utp_rendezvous_registered_decode(utp_rendezvous_registered_t* registered, const uint8_t* buffer,
                                                      size_t length)
{
    utp_wire_reader_t           reader;
    utp_rendezvous_registered_t decoded = {0};
    utp_internal_error_t        error;
    size_t                      index;
    size_t                      address_length;

    if (registered == NULL || buffer == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_wire_reader_init(&reader, buffer, length);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u64(&reader, &decoded.registration_request_id);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_rendezvous_read_bytes(&reader, decoded.registration_token, sizeof(decoded.registration_token));
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u64(&reader, &decoded.calibration_id);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u8(&reader, &decoded.calibration_endpoint_count);
    }
    if (error != UTP_INTERNAL_ERROR_OK || decoded.calibration_endpoint_count > UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES ||
        (decoded.calibration_id == 0u && decoded.calibration_endpoint_count != 0u) ||
        (decoded.calibration_id != 0u && decoded.calibration_endpoint_count == 0u)) {
        return error == UTP_INTERNAL_ERROR_OK ? UTP_INTERNAL_ERROR_PROTOCOL : error;
    }
    for (index = 0u; index < decoded.calibration_endpoint_count; ++index) {
        utp_address_t* const endpoint = &decoded.decoded_calibration_endpoints[index];

        if (utp_wire_read_u8(&reader, &endpoint->family) != UTP_INTERNAL_ERROR_OK ||
            utp_wire_read_u16(&reader, &endpoint->port) != UTP_INTERNAL_ERROR_OK) {
            return UTP_INTERNAL_ERROR_OVERFLOW;
        }
        address_length = utp_rendezvous_address_length(endpoint->family);
        if (endpoint->port == 0u || address_length == 0u || reader.remaining < address_length) {
            return UTP_INTERNAL_ERROR_PROTOCOL;
        }
        error = utp_rendezvous_read_bytes(&reader, endpoint->address, address_length);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
    }
    if (reader.remaining != 0u) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    decoded.calibration_endpoints     = decoded.decoded_calibration_endpoints;
    *registered                       = decoded;
    registered->calibration_endpoints = registered->decoded_calibration_endpoints;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_rendezvous_ping_encode(uint8_t* buffer, size_t capacity, const utp_rendezvous_ping_t* ping)
{
    utp_wire_writer_t    writer;
    utp_internal_error_t error;

    if (buffer == NULL || ping == NULL ||
        capacity < UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE + sizeof(ping->calibration_id)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_wire_writer_init(&writer, buffer, capacity);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_rendezvous_write_bytes(&writer, ping->registration_token, sizeof(ping->registration_token));
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u64(&writer, ping->calibration_id);
    }
    return error;
}

utp_internal_error_t utp_rendezvous_ping_decode(utp_rendezvous_ping_t* ping, const uint8_t* buffer, size_t length)
{
    utp_wire_reader_t    reader;
    utp_internal_error_t error;

    if (ping == NULL || buffer == NULL ||
        length != UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE + sizeof(ping->calibration_id)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_wire_reader_init(&reader, buffer, length);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_rendezvous_read_bytes(&reader, ping->registration_token, sizeof(ping->registration_token));
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u64(&reader, &ping->calibration_id);
    }
    return error;
}

utp_internal_error_t utp_rendezvous_pong_encode(uint8_t* buffer, size_t capacity, const utp_rendezvous_pong_t* pong)
{
    utp_wire_writer_t    writer;
    utp_internal_error_t error;

    if (buffer == NULL || pong == NULL || pong->acknowledged_packet_number == 0u ||
        capacity < UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE + sizeof(pong->acknowledged_packet_number)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_wire_writer_init(&writer, buffer, capacity);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_rendezvous_write_bytes(&writer, pong->registration_token, sizeof(pong->registration_token));
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u64(&writer, pong->acknowledged_packet_number);
    }
    return error;
}

utp_internal_error_t utp_rendezvous_pong_decode(utp_rendezvous_pong_t* pong, const uint8_t* buffer, size_t length)
{
    utp_wire_reader_t    reader;
    utp_internal_error_t error;

    if (pong == NULL || buffer == NULL ||
        length != UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE + sizeof(pong->acknowledged_packet_number)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    error = utp_wire_reader_init(&reader, buffer, length);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_rendezvous_read_bytes(&reader, pong->registration_token, sizeof(pong->registration_token));
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u64(&reader, &pong->acknowledged_packet_number);
    }
    return error == UTP_INTERNAL_ERROR_OK && pong->acknowledged_packet_number == 0u ? UTP_INTERNAL_ERROR_PROTOCOL
                                                                                    : error;
}

static utp_internal_error_t utp_rendezvous_validate_candidate_plan(const utp_rendezvous_candidate_plan_t* plan)
{
    size_t address_length;
    size_t index;

    if (plan->local_port == 0u || plan->local_candidate_count > UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES ||
        plan->public_port_count == 0u || plan->public_port_count > UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES ||
        (plan->local_candidate_count != 0u && plan->local_candidates == NULL)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    address_length = utp_rendezvous_address_length(plan->family);
    if (address_length == 0u || plan->public_address.family != plan->family || plan->public_address.port != 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    for (index = 0u; index < plan->local_candidate_count; ++index) {
        if (plan->local_candidates[index].family != plan->family ||
            plan->local_candidates[index].port != plan->local_port) {
            return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
        }
    }
    for (index = 0u; index < plan->public_port_count; ++index) {
        if (plan->public_ports[index] == 0u ||
            (index != 0u && plan->public_ports[index - 1u] == plan->public_ports[index])) {
            return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
        }
    }
    return UTP_INTERNAL_ERROR_OK;
}

static utp_internal_error_t utp_rendezvous_validate_request(const utp_rendezvous_request_t* request)
{
    size_t address_length;
    size_t index;

    if (request->source_peer_id == NULL || request->target_peer_id == NULL || request->source_peer_id_length == 0u ||
        request->source_peer_id_length > UTP_PEER_ID_MAX_LENGTH || request->target_peer_id_length == 0u ||
        request->target_peer_id_length > UTP_PEER_ID_MAX_LENGTH || request->local_port == 0u ||
        request->local_candidate_count > UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES ||
        (request->local_candidate_count != 0u && request->local_candidates == NULL)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    address_length = utp_rendezvous_address_length(request->local_family);
    if (address_length == 0u) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    for (index = 0u; index < request->local_candidate_count; ++index) {
        if (request->local_candidates[index].family != request->local_family) {
            return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
        }
    }
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_rendezvous_request_encode(uint8_t* buffer, size_t capacity,
                                                   const utp_rendezvous_request_t* request, size_t* out_length)
{
    utp_wire_writer_t    writer;
    utp_internal_error_t error;
    size_t               address_length;
    size_t               index;
    size_t               address_index;

    error = utp_rendezvous_validate_request(request);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    address_length = utp_rendezvous_address_length(request->local_family);
    error          = utp_wire_writer_init(&writer, buffer, capacity);
    for (index = 0u; index < sizeof(request->rendezvous_id) && error == UTP_INTERNAL_ERROR_OK; ++index) {
        error = utp_wire_write_u8(&writer, request->rendezvous_id[index]);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u8(&writer, request->source_peer_id_length);
    }
    for (index = 0u; index < request->source_peer_id_length && error == UTP_INTERNAL_ERROR_OK; ++index) {
        error = utp_wire_write_u8(&writer, request->source_peer_id[index]);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u8(&writer, request->target_peer_id_length);
    }
    for (index = 0u; index < request->target_peer_id_length && error == UTP_INTERNAL_ERROR_OK; ++index) {
        error = utp_wire_write_u8(&writer, request->target_peer_id[index]);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u8(&writer, request->source_nat_class);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u8(&writer, request->local_family);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u16(&writer, request->local_port);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u8(&writer, request->local_candidate_count);
    }
    for (index = 0u; index < request->local_candidate_count && error == UTP_INTERNAL_ERROR_OK; ++index) {
        for (address_index = 0u; address_index < address_length && error == UTP_INTERNAL_ERROR_OK; ++address_index) {
            error = utp_wire_write_u8(&writer, request->local_candidates[index].address[address_index]);
        }
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        *out_length = capacity - writer.remaining;
    }
    return error;
}

utp_internal_error_t utp_rendezvous_request_decode(utp_rendezvous_request_t* request, const uint8_t* buffer,
                                                   size_t length)
{
    utp_wire_reader_t        reader;
    utp_rendezvous_request_t decoded = {0};
    size_t                   address_length;
    size_t                   index;
    size_t                   address_index;
    utp_internal_error_t     error;

    error = utp_wire_reader_init(&reader, buffer, length);
    for (index = 0u; index < sizeof(decoded.rendezvous_id) && error == UTP_INTERNAL_ERROR_OK; ++index) {
        error = utp_wire_read_u8(&reader, &decoded.rendezvous_id[index]);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u8(&reader, &decoded.source_peer_id_length);
    }
    if (error != UTP_INTERNAL_ERROR_OK || decoded.source_peer_id_length == 0u ||
        decoded.source_peer_id_length > UTP_PEER_ID_MAX_LENGTH || reader.remaining < decoded.source_peer_id_length) {
        return error == UTP_INTERNAL_ERROR_OK ? UTP_INTERNAL_ERROR_PROTOCOL : error;
    }
    decoded.source_peer_id  = reader.cursor;
    reader.cursor          += decoded.source_peer_id_length;
    reader.remaining       -= decoded.source_peer_id_length;
    error                   = utp_wire_read_u8(&reader, &decoded.target_peer_id_length);
    if (error != UTP_INTERNAL_ERROR_OK || decoded.target_peer_id_length == 0u ||
        decoded.target_peer_id_length > UTP_PEER_ID_MAX_LENGTH || reader.remaining < decoded.target_peer_id_length) {
        return error == UTP_INTERNAL_ERROR_OK ? UTP_INTERNAL_ERROR_PROTOCOL : error;
    }
    decoded.target_peer_id  = reader.cursor;
    reader.cursor          += decoded.target_peer_id_length;
    reader.remaining       -= decoded.target_peer_id_length;
    error                   = utp_wire_read_u8(&reader, &decoded.source_nat_class);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u8(&reader, &decoded.local_family);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u16(&reader, &decoded.local_port);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u8(&reader, &decoded.local_candidate_count);
    }
    if (error != UTP_INTERNAL_ERROR_OK || decoded.local_port == 0u ||
        decoded.local_candidate_count > UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES) {
        return error == UTP_INTERNAL_ERROR_OK ? UTP_INTERNAL_ERROR_PROTOCOL : error;
    }
    address_length = utp_rendezvous_address_length(decoded.local_family);
    if (address_length == 0u || reader.remaining != address_length * (size_t)decoded.local_candidate_count) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    for (index = 0u; index < decoded.local_candidate_count; ++index) {
        decoded.decoded_local_candidates[index].family = decoded.local_family;
        decoded.decoded_local_candidates[index].port   = decoded.local_port;
        for (address_index = 0u; address_index < address_length; ++address_index) {
            error = utp_wire_read_u8(&reader, &decoded.decoded_local_candidates[index].address[address_index]);
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
        }
    }
    *request                  = decoded;
    request->local_candidates = request->decoded_local_candidates;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_rendezvous_candidate_plan_encode(uint8_t* buffer, size_t capacity,
                                                          const utp_rendezvous_candidate_plan_t* plan,
                                                          size_t*                                out_length)
{
    utp_wire_writer_t    writer;
    utp_internal_error_t error;
    size_t               address_length;
    size_t               index;
    size_t               address_index;

    error = utp_rendezvous_validate_candidate_plan(plan);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    address_length = utp_rendezvous_address_length(plan->family);
    error          = utp_wire_writer_init(&writer, buffer, capacity);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u8(&writer, plan->family);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u16(&writer, plan->local_port);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u8(&writer, plan->local_candidate_count);
    }
    for (index = 0u; index < plan->local_candidate_count && error == UTP_INTERNAL_ERROR_OK; ++index) {
        for (address_index = 0u; address_index < address_length && error == UTP_INTERNAL_ERROR_OK; ++address_index) {
            error = utp_wire_write_u8(&writer, plan->local_candidates[index].address[address_index]);
        }
    }
    for (address_index = 0u; address_index < address_length && error == UTP_INTERNAL_ERROR_OK; ++address_index) {
        error = utp_wire_write_u8(&writer, plan->public_address.address[address_index]);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_write_u8(&writer, plan->public_port_count);
    }
    for (index = 0u; index < plan->public_port_count && error == UTP_INTERNAL_ERROR_OK; ++index) {
        error = utp_wire_write_u16(&writer, plan->public_ports[index]);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        *out_length = capacity - writer.remaining;
    }
    return error;
}

utp_internal_error_t utp_rendezvous_candidate_plan_decode(utp_rendezvous_candidate_plan_t* plan, const uint8_t* buffer,
                                                          size_t length, size_t* consumed)
{
    utp_wire_reader_t               reader;
    utp_rendezvous_candidate_plan_t decoded = {0};
    utp_internal_error_t            error;
    size_t                          address_length;
    size_t                          index;
    size_t                          address_index;

    error = utp_wire_reader_init(&reader, buffer, length);
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u8(&reader, &decoded.family);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u16(&reader, &decoded.local_port);
    }
    if (error == UTP_INTERNAL_ERROR_OK) {
        error = utp_wire_read_u8(&reader, &decoded.local_candidate_count);
    }
    address_length = utp_rendezvous_address_length(decoded.family);
    if (error != UTP_INTERNAL_ERROR_OK || decoded.local_port == 0u || address_length == 0u ||
        decoded.local_candidate_count > UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES ||
        reader.remaining < address_length * (size_t)decoded.local_candidate_count + address_length + 1u) {
        return error == UTP_INTERNAL_ERROR_OK ? UTP_INTERNAL_ERROR_PROTOCOL : error;
    }
    for (index = 0u; index < decoded.local_candidate_count; ++index) {
        decoded.decoded_local_candidates[index].family = decoded.family;
        decoded.decoded_local_candidates[index].port   = decoded.local_port;
        for (address_index = 0u; address_index < address_length; ++address_index) {
            error = utp_wire_read_u8(&reader, &decoded.decoded_local_candidates[index].address[address_index]);
            if (error != UTP_INTERNAL_ERROR_OK) {
                return error;
            }
        }
    }
    decoded.public_address.family = decoded.family;
    for (address_index = 0u; address_index < address_length; ++address_index) {
        error = utp_wire_read_u8(&reader, &decoded.public_address.address[address_index]);
        if (error != UTP_INTERNAL_ERROR_OK) {
            return error;
        }
    }
    error = utp_wire_read_u8(&reader, &decoded.public_port_count);
    if (error != UTP_INTERNAL_ERROR_OK || decoded.public_port_count == 0u ||
        decoded.public_port_count > UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES ||
        reader.remaining < (size_t)decoded.public_port_count * 2u) {
        return error == UTP_INTERNAL_ERROR_OK ? UTP_INTERNAL_ERROR_PROTOCOL : error;
    }
    for (index = 0u; index < decoded.public_port_count; ++index) {
        error = utp_wire_read_u16(&reader, &decoded.public_ports[index]);
        if (error != UTP_INTERNAL_ERROR_OK || decoded.public_ports[index] == 0u ||
            (index != 0u && decoded.public_ports[index - 1u] == decoded.public_ports[index])) {
            return error == UTP_INTERNAL_ERROR_OK ? UTP_INTERNAL_ERROR_PROTOCOL : error;
        }
    }
    decoded.local_candidates = decoded.decoded_local_candidates;
    *consumed                = length - reader.remaining;
    *plan                    = decoded;
    plan->local_candidates   = plan->decoded_local_candidates;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_rendezvous_redirect_encode(uint8_t* buffer, size_t capacity,
                                                    const utp_rendezvous_redirect_t* redirect, size_t* out_length)
{
    size_t               plan_length;
    utp_internal_error_t error;

    if (capacity < UTP_RENDEZVOUS_ID_SIZE) {
        return UTP_INTERNAL_ERROR_OVERFLOW;
    }
    memcpy(buffer, redirect->rendezvous_id, UTP_RENDEZVOUS_ID_SIZE);
    error = utp_rendezvous_candidate_plan_encode(buffer + UTP_RENDEZVOUS_ID_SIZE, capacity - UTP_RENDEZVOUS_ID_SIZE,
                                                 &redirect->target_plan, &plan_length);
    if (error == UTP_INTERNAL_ERROR_OK) {
        *out_length = UTP_RENDEZVOUS_ID_SIZE + plan_length;
    }
    return error;
}

utp_internal_error_t utp_rendezvous_redirect_decode(utp_rendezvous_redirect_t* redirect, const uint8_t* buffer,
                                                    size_t length)
{
    size_t               consumed;
    utp_internal_error_t error;

    if (length < UTP_RENDEZVOUS_ID_SIZE) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    *redirect = (utp_rendezvous_redirect_t){0};
    memcpy(redirect->rendezvous_id, buffer, UTP_RENDEZVOUS_ID_SIZE);
    error = utp_rendezvous_candidate_plan_decode(&redirect->target_plan, buffer + UTP_RENDEZVOUS_ID_SIZE,
                                                 length - UTP_RENDEZVOUS_ID_SIZE, &consumed);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    return consumed == length - UTP_RENDEZVOUS_ID_SIZE ? UTP_INTERNAL_ERROR_OK : UTP_INTERNAL_ERROR_PROTOCOL;
}

utp_internal_error_t utp_rendezvous_forward_encode(uint8_t* buffer, size_t capacity,
                                                   const utp_rendezvous_forward_t* forward, size_t* out_length)
{
    size_t               plan_length;
    utp_internal_error_t error;

    if (forward->source_peer_id == NULL || forward->source_peer_id_length == 0u ||
        forward->source_peer_id_length > UTP_PEER_ID_MAX_LENGTH ||
        capacity < UTP_RENDEZVOUS_ID_SIZE + 1u + forward->source_peer_id_length) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    memcpy(buffer, forward->rendezvous_id, UTP_RENDEZVOUS_ID_SIZE);
    buffer[UTP_RENDEZVOUS_ID_SIZE] = forward->source_peer_id_length;
    memcpy(buffer + UTP_RENDEZVOUS_ID_SIZE + 1u, forward->source_peer_id, forward->source_peer_id_length);
    error = utp_rendezvous_candidate_plan_encode(
        buffer + UTP_RENDEZVOUS_ID_SIZE + 1u + forward->source_peer_id_length,
        capacity - UTP_RENDEZVOUS_ID_SIZE - 1u - forward->source_peer_id_length, &forward->source_plan, &plan_length);
    if (error == UTP_INTERNAL_ERROR_OK) {
        *out_length = UTP_RENDEZVOUS_ID_SIZE + 1u + forward->source_peer_id_length + plan_length;
    }
    return error;
}

utp_internal_error_t utp_rendezvous_forward_decode(utp_rendezvous_forward_t* forward, const uint8_t* buffer,
                                                   size_t length)
{
    size_t               consumed;
    utp_internal_error_t error;

    if (length <= UTP_RENDEZVOUS_ID_SIZE) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    *forward = (utp_rendezvous_forward_t){0};
    memcpy(forward->rendezvous_id, buffer, UTP_RENDEZVOUS_ID_SIZE);
    forward->source_peer_id_length = buffer[UTP_RENDEZVOUS_ID_SIZE];
    if (forward->source_peer_id_length == 0u || forward->source_peer_id_length > UTP_PEER_ID_MAX_LENGTH ||
        length <= UTP_RENDEZVOUS_ID_SIZE + 1u + forward->source_peer_id_length) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    forward->source_peer_id = buffer + UTP_RENDEZVOUS_ID_SIZE + 1u;
    error                   = utp_rendezvous_candidate_plan_decode(
        &forward->source_plan, forward->source_peer_id + forward->source_peer_id_length,
        length - UTP_RENDEZVOUS_ID_SIZE - 1u - forward->source_peer_id_length, &consumed);
    if (error != UTP_INTERNAL_ERROR_OK) {
        return error;
    }
    return consumed == length - UTP_RENDEZVOUS_ID_SIZE - 1u - forward->source_peer_id_length
               ? UTP_INTERNAL_ERROR_OK
               : UTP_INTERNAL_ERROR_PROTOCOL;
}

utp_internal_error_t utp_rendezvous_introduction_decode(uint8_t        rendezvous_id[UTP_RENDEZVOUS_ID_SIZE],
                                                        const uint8_t* buffer, size_t length)
{
    if (length != UTP_RENDEZVOUS_ID_SIZE) {
        return UTP_INTERNAL_ERROR_PROTOCOL;
    }
    memcpy(rendezvous_id, buffer, UTP_RENDEZVOUS_ID_SIZE);
    return UTP_INTERNAL_ERROR_OK;
}
