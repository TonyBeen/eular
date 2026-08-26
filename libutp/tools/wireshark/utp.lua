-- Eular UTP Wireshark Lua dissector
-- Protocol layout comes from c/src/proto, c/src/nat and c/src/rendezvous.

local utp = Proto("UTP", "Eular UTP")

local packet_type_names = {
    [0x00] = "NONE",
    [0x01] = "INITIAL",
    [0x02] = "HANDSHAKE",
    [0x03] = "0RTT",
    [0x04] = "CONNECTION_CLOSE",
    [0x05] = "CTRL",
    [0x06] = "RENDEZVOUS",
    [0x07] = "NAT_PROBE",
}

local frame_type_names = {
    [0] = "Invalid",
    [1] = "Stream",
    [2] = "Ack",
    [3] = "Padding",
    [4] = "ConnectionClose",
    [5] = "Ping",
    [6] = "ResetStream",
    [7] = "StreamsBlocked",
    [8] = "MaxStreams",
    [9] = "PathChallenge",
    [10] = "PathResponse",
    [11] = "Crypto",
    [12] = "SessionToken",
    [13] = "AckFrequency",
    [14] = "Version",
    [15] = "HandshakeDone",
    [16] = "TransportParams",
    [17] = "HandshakeDelay",
    [18] = "MaxData",
    [19] = "MaxStreamData",
    [20] = "DataBlocked",
    [21] = "StreamDataBlocked",
    [22] = "StopSending",
    [23] = "Rendezvous",
    [24] = "ObservedAddress",
}

local rendezvous_message_names = {
    [1] = "REGISTER",
    [2] = "REGISTERED",
    [3] = "PING",
    [4] = "PONG",
    [5] = "CALIBRATE",
    [6] = "ADDRESS_UPDATE",
    [7] = "ADDRESS_UPDATED",
    [8] = "REQUEST",
    [9] = "REDIRECT",
    [10] = "FORWARD",
    [11] = "INTRODUCTION",
    [12] = "UNREGISTER",
    [13] = "UNREGISTERED",
    [14] = "REJECTED",
}

local nat_message_names = {
    [1] = "BindingRequest",
    [2] = "BindingResponse",
}

local nat_step_names = {
    [1] = "PrimaryBinding",
    [2] = "AlternateBinding",
}

local nat_change_names = {
    [0] = "None",
    [1] = "ChangePort",
    [3] = "ChangePort|ChangeIP",
}

local nat_tlv_names = {
    [1] = "ProbeToken",
    [6] = "MappedAddress",
    [7] = "OriginAddress",
    [8] = "AlternateProbeEndpoint",
    [12] = "Padding",
}

local nat_class_names = {
    [0] = "Unknown",
    [1] = "OpenPublic",
    [2] = "OpenPublicWithFirewall",
    [3] = "FullCone",
    [4] = "IpRestricted",
    [5] = "PortRestricted",
    [6] = "Symmetric",
    [7] = "SymmetricMultiLine",
    [8] = "UdpBlocked",
}

utp.prefs.udp_port = Pref.uint("UDP port", 9000, "UTP UDP port (0 disables auto registration)")

local f = utp.fields
f.scid = ProtoField.uint32("eular_utp.scid", "SCID", base.HEX)
f.dcid = ProtoField.uint32("eular_utp.dcid", "DCID", base.HEX)
f.pn = ProtoField.uint64("eular_utp.pn", "Packet Number", base.DEC)
f.payload_len = ProtoField.uint16("eular_utp.payload_len", "Payload Length", base.DEC)
f.packet_len = ProtoField.uint16("eular_utp.packet_len", "UTP Packet Length", base.DEC)
f.frame_count = ProtoField.uint16("eular_utp.frame_count", "Frame Count", base.DEC)
f.packet_type = ProtoField.uint8("eular_utp.packet_type", "Packet Type", base.HEX, packet_type_names)
f.reserve = ProtoField.uint8("eular_utp.reserve", "Reserve", base.HEX)
f.payload_raw = ProtoField.bytes("eular_utp.payload.raw", "Payload Raw")
f.payload_cipher = ProtoField.bytes("eular_utp.payload.cipher", "Encrypted Payload")
f.payload_tag = ProtoField.bytes("eular_utp.payload.tag", "GCM Tag")
f.payload_undecoded = ProtoField.bytes("eular_utp.payload.undecoded", "Undecoded Payload Tail")

f.frame_index = ProtoField.uint16("eular_utp.frame.index", "Frame Index", base.DEC)
f.frame_type = ProtoField.uint8("eular_utp.frame.type", "Frame Type", base.DEC, frame_type_names)
f.frame_len = ProtoField.uint16("eular_utp.frame.len", "Frame Length", base.DEC)

f.stream_flag = ProtoField.uint8("eular_utp.stream.flag", "Stream Flag", base.HEX)
f.stream_fin = ProtoField.bool("eular_utp.stream.fin", "FIN", 8, nil, 0x01)
f.stream_data_len = ProtoField.uint16("eular_utp.stream.data_len", "Stream Data Length", base.DEC)
f.stream_id = ProtoField.uint32("eular_utp.stream.id", "Stream ID", base.DEC)
f.stream_offset = ProtoField.uint64("eular_utp.stream.offset", "Stream Offset", base.DEC)
f.stream_data = ProtoField.bytes("eular_utp.stream.data", "Stream Data")

f.stop_sending_error = ProtoField.uint16("eular_utp.stop_sending.error", "Stop Sending Error Code", base.DEC)
f.stop_sending_stream_id = ProtoField.uint32("eular_utp.stop_sending.stream_id", "Stop Sending Stream ID", base.DEC)
f.stream_limit_type = ProtoField.uint8("eular_utp.stream_limit.type", "Stream Type", base.DEC, {
    [0] = "Bidirectional",
    [1] = "Unidirectional",
})
f.stream_limit_value = ProtoField.uint16("eular_utp.stream_limit.value", "Stream Limit", base.DEC)

f.ack_count = ProtoField.uint8("eular_utp.ack.count", "Ack Range Count", base.DEC)
f.ack_delay = ProtoField.uint16("eular_utp.ack.delay", "Ack Delay", base.DEC)
f.ack_first_range = ProtoField.uint32("eular_utp.ack.first_range", "First Ack Range", base.DEC)
f.ack_largest = ProtoField.uint64("eular_utp.ack.largest", "Ack Largest", base.DEC)
f.ack_gap = ProtoField.uint32("eular_utp.ack.range.gap", "Ack Gap", base.DEC)
f.ack_range_len = ProtoField.uint32("eular_utp.ack.range.len", "Ack Range Length", base.DEC)
f.ack_abs_low = ProtoField.uint64("eular_utp.ack.range.abs_low", "Absolute Ack Range Low", base.DEC)
f.ack_abs_high = ProtoField.uint64("eular_utp.ack.range.abs_high", "Absolute Ack Range High", base.DEC)

f.padding_len = ProtoField.uint16("eular_utp.padding.len", "Padding Length", base.DEC)

f.close_error = ProtoField.uint16("eular_utp.close.error", "Close Error Code", base.DEC)
f.close_reason_len = ProtoField.uint16("eular_utp.close.reason_len", "Close Reason Length", base.DEC)
f.close_reason = ProtoField.string("eular_utp.close.reason", "Close Reason")

f.path_data = ProtoField.bytes("eular_utp.path.data", "Path Data")

f.crypto_type = ProtoField.uint8("eular_utp.crypto.type", "Crypto Type", base.DEC)
f.crypto_reserved = ProtoField.uint8("eular_utp.crypto.reserved", "Reserved", base.HEX)
f.crypto_pubkey = ProtoField.bytes("eular_utp.crypto.pubkey", "Ephemeral Public Key")

f.token_size = ProtoField.uint8("eular_utp.token.size", "Token Size", base.DEC)
f.token_validity = ProtoField.uint16("eular_utp.token.validity", "Token Validity Period", base.DEC)
f.token_data = ProtoField.bytes("eular_utp.token.data", "Token")

f.ack_freq_thresh = ProtoField.uint8("eular_utp.ack_frequency.ack_eliciting_threshold", "Ack Eliciting Threshold", base.DEC)
f.ack_freq_reorder = ProtoField.uint8("eular_utp.ack_frequency.reordering_threshold", "Reordering Threshold", base.DEC)
f.ack_freq_max_delay = ProtoField.uint32("eular_utp.ack_frequency.max_ack_delay_ms", "Max Ack Delay (ms)", base.DEC)
f.handshake_done_ack_pn = ProtoField.uint64("eular_utp.handshake_done.ack_handshake_pn", "Acked Handshake Packet Number", base.DEC)
f.handshake_delay_us = ProtoField.uint32("eular_utp.handshake_delay.delay_time_us", "Handshake Delay (us)", base.DEC)

f.version = ProtoField.uint32("eular_utp.version", "Version", base.DEC)

f.tp_flags = ProtoField.uint16("eular_utp.transport_params.flags", "TransportParams Flags", base.HEX)
f.tp_max_idle_timeout = ProtoField.uint32("eular_utp.transport_params.max_idle_timeout", "Max Idle Timeout (ms)", base.DEC)
f.tp_handshake_timeout = ProtoField.uint16("eular_utp.transport_params.handshake_timeout", "Handshake Timeout (ms)", base.DEC)
f.tp_init_max_streams_bidi = ProtoField.uint16("eular_utp.transport_params.init_max_streams_bidi", "Init Max Streams Bidi", base.DEC)
f.tp_init_max_streams_uni = ProtoField.uint16("eular_utp.transport_params.init_max_streams_uni", "Init Max Streams Uni", base.DEC)
f.tp_ack_delay_exponent = ProtoField.uint8("eular_utp.transport_params.ack_delay_exponent", "Ack Delay Exponent", base.DEC)
f.tp_initial_max_data = ProtoField.uint64("eular_utp.transport_params.initial_max_data", "Initial Max Data", base.DEC)
f.tp_initial_max_stream_data_bidi_local = ProtoField.uint64("eular_utp.transport_params.initial_max_stream_data_bidi_local", "Initial Max Stream Data Bidi Local", base.DEC)
f.tp_initial_max_stream_data_bidi_remote = ProtoField.uint64("eular_utp.transport_params.initial_max_stream_data_bidi_remote", "Initial Max Stream Data Bidi Remote", base.DEC)

f.reset_error = ProtoField.uint16("eular_utp.reset.error", "Reset Error Code", base.DEC)
f.reset_stream_id = ProtoField.uint32("eular_utp.reset.stream_id", "Reset Stream ID", base.DEC)
f.reset_final_size = ProtoField.uint64("eular_utp.reset.final_size", "Reset Final Size", base.DEC)

f.max_data_limit = ProtoField.uint64("eular_utp.max_data.maximum_data", "Maximum Data", base.DEC)
f.max_stream_data_stream_id = ProtoField.uint32("eular_utp.max_stream_data.stream_id", "MaxStreamData Stream ID", base.DEC)
f.max_stream_data_limit = ProtoField.uint64("eular_utp.max_stream_data.maximum_stream_data", "Maximum Stream Data", base.DEC)
f.data_blocked_limit = ProtoField.uint64("eular_utp.data_blocked.data_limit", "Data Limit", base.DEC)
f.stream_data_blocked_stream_id = ProtoField.uint32("eular_utp.stream_data_blocked.stream_id", "StreamDataBlocked Stream ID", base.DEC)
f.stream_data_blocked_limit = ProtoField.uint64("eular_utp.stream_data_blocked.stream_data_limit", "Stream Data Limit", base.DEC)

f.observed_family = ProtoField.uint8("eular_utp.observed_address.family", "Address Family", base.DEC, {
    [4] = "IPv4",
    [6] = "IPv6",
})
f.observed_port = ProtoField.uint16("eular_utp.observed_address.port", "Observed Port", base.DEC)
f.observed_address = ProtoField.string("eular_utp.observed_address.address", "Observed Address")

f.rendezvous_message_type = ProtoField.uint8("eular_utp.rendezvous.message_type", "Rendezvous Message", base.DEC,
    rendezvous_message_names)
f.rendezvous_payload_len = ProtoField.uint16("eular_utp.rendezvous.payload_len", "Rendezvous Payload Length", base.DEC)
f.rendezvous_payload = ProtoField.bytes("eular_utp.rendezvous.payload", "Rendezvous Payload")
f.rendezvous_id = ProtoField.bytes("eular_utp.rendezvous.id", "Rendezvous ID")
f.rendezvous_source_peer_id = ProtoField.string("eular_utp.rendezvous.source_peer_id", "Source Peer ID")
f.rendezvous_target_peer_id = ProtoField.string("eular_utp.rendezvous.target_peer_id", "Target Peer ID")
f.rendezvous_nat_class = ProtoField.uint8("eular_utp.rendezvous.nat_class", "Source NAT Class", base.DEC,
    nat_class_names)
f.rendezvous_family = ProtoField.uint8("eular_utp.rendezvous.family", "Address Family", base.DEC, {
    [4] = "IPv4",
    [6] = "IPv6",
})
f.rendezvous_local_port = ProtoField.uint16("eular_utp.rendezvous.local_port", "Local Port", base.DEC)
f.rendezvous_local_candidate_count = ProtoField.uint8("eular_utp.rendezvous.local_candidate_count",
    "Local Candidate Count", base.DEC)
f.rendezvous_local_candidate = ProtoField.string("eular_utp.rendezvous.local_candidate", "Local Candidate")
f.rendezvous_public_address = ProtoField.string("eular_utp.rendezvous.public_address", "Public Address")
f.rendezvous_public_port_count = ProtoField.uint8("eular_utp.rendezvous.public_port_count", "Public Port Count", base.DEC)
f.rendezvous_public_port = ProtoField.uint16("eular_utp.rendezvous.public_port", "Public Port Candidate", base.DEC)

f.nat_version = ProtoField.uint8("eular_utp.nat.version", "NAT Probe Version", base.DEC)
f.nat_message_type = ProtoField.uint8("eular_utp.nat.message_type", "NAT Probe Message", base.DEC, nat_message_names)
f.nat_step = ProtoField.uint8("eular_utp.nat.step", "NAT Probe Step", base.DEC, nat_step_names)
f.nat_change_flags = ProtoField.uint8("eular_utp.nat.change_flags", "Requested/Response Change", base.HEX,
    nat_change_names)
f.nat_tlv_type = ProtoField.uint16("eular_utp.nat.tlv.type", "NAT TLV Type", base.DEC, nat_tlv_names)
f.nat_tlv_len = ProtoField.uint16("eular_utp.nat.tlv.len", "NAT TLV Length", base.DEC)
f.nat_tlv_value = ProtoField.bytes("eular_utp.nat.tlv.value", "NAT TLV Value")
f.nat_token = ProtoField.bytes("eular_utp.nat.token", "Probe Token")
f.nat_endpoint_family = ProtoField.uint8("eular_utp.nat.endpoint.family", "Endpoint Family", base.DEC, {
    [4] = "IPv4",
    [6] = "IPv6",
})
f.nat_endpoint_port = ProtoField.uint16("eular_utp.nat.endpoint.port", "Endpoint Port", base.DEC)
f.nat_endpoint_address = ProtoField.string("eular_utp.nat.endpoint.address", "Endpoint Address")

local function frame_name(ftype)
    return frame_type_names[ftype] or string.format("Unknown(%d)", ftype)
end

local function packet_type_name(ptype)
    return packet_type_names[ptype] or string.format("Unknown(0x%02x)", ptype)
end

local function packet_can_be_encrypted(ptype)
    -- INITIAL/HANDSHAKE 明文；0RTT/CONNECTION_CLOSE/CTRL 在建链后通常为密文。
    return ptype == 0x03 or ptype == 0x04 or ptype == 0x05
end

local function address_length(family)
    if family == 4 then
        return 4
    end
    if family == 6 then
        return 16
    end
    return nil
end

local function address_text(payload, offset, family)
    local length = address_length(family)
    if length == nil or offset + length > payload:len() then
        return nil
    end
    if family == 4 then
        return string.format("%u.%u.%u.%u", payload(offset, 1):uint(), payload(offset + 1, 1):uint(),
            payload(offset + 2, 1):uint(), payload(offset + 3, 1):uint())
    end

    local groups = {}
    local best_start = nil
    local best_length = 0
    local current_start = nil
    local current_length = 0
    for index = 0, 7 do
        local group = payload(offset + index * 2, 2):uint()
        groups[index + 1] = group
        if group == 0 then
            if current_start == nil then
                current_start = index + 1
                current_length = 1
            else
                current_length = current_length + 1
            end
        else
            if current_length > best_length then
                best_start = current_start
                best_length = current_length
            end
            current_start = nil
            current_length = 0
        end
    end
    if current_length > best_length then
        best_start = current_start
        best_length = current_length
    end
    if best_length < 2 then
        best_start = nil
    end

    local parts = {}
    local index = 1
    while index <= 8 do
        if best_start ~= nil and index == best_start then
            parts[#parts + 1] = ""
            index = index + best_length
            if index > 8 then
                parts[#parts + 1] = ""
            end
        else
            parts[#parts + 1] = string.format("%x", groups[index])
            index = index + 1
        end
    end
    local text = table.concat(parts, ":")
    if best_start == 1 then
        text = ":" .. text
    end
    return text
end

local function endpoint_text(payload, offset, family, port)
    local address = address_text(payload, offset, family)
    if address == nil then
        return nil
    end
    if family == 6 then
        return string.format("[%s]:%u", address, port)
    end
    return string.format("%s:%u", address, port)
end

local function rendezvous_message_name(message_type)
    return rendezvous_message_names[message_type] or string.format("Unknown(%u)", message_type)
end

local function nat_message_name(message_type)
    return nat_message_names[message_type] or string.format("Unknown(%u)", message_type)
end

local function nat_step_name(step)
    return nat_step_names[step] or string.format("Unknown(%u)", step)
end

local function nat_change_name(change_flags)
    return nat_change_names[change_flags] or string.format("Unknown(0x%02x)", change_flags)
end

local function append_summary(summaries, text)
    if text == nil or text == "" then
        return
    end
    if #summaries >= 4 then
        return
    end
    summaries[#summaries + 1] = text
end

local function parse_rendezvous_candidate_plan(payload, offset, payload_len, tree)
    if offset + 4 > payload_len then
        return nil, "truncated CandidatePlan header"
    end
    local family = payload(offset, 1):uint()
    local local_port = payload(offset + 1, 2):uint()
    local candidate_count = payload(offset + 3, 1):uint()
    local addr_len = address_length(family)
    if addr_len == nil or candidate_count > 4 then
        return nil, "invalid CandidatePlan family or candidate count"
    end
    local candidates_offset = offset + 4
    local public_address_offset = candidates_offset + candidate_count * addr_len
    local port_count_offset = public_address_offset + addr_len
    if port_count_offset + 1 > payload_len then
        return nil, "truncated CandidatePlan addresses"
    end
    local public_port_count = payload(port_count_offset, 1):uint()
    local ports_offset = port_count_offset + 1
    local end_offset = ports_offset + public_port_count * 2
    if public_port_count == 0 or public_port_count > 4 or end_offset > payload_len then
        return nil, "invalid CandidatePlan public port count"
    end

    local plan = tree:add(payload(offset, end_offset - offset), "Candidate Plan")
    plan:add(f.rendezvous_family, payload(offset, 1))
    plan:add(f.rendezvous_local_port, payload(offset + 1, 2))
    plan:add(f.rendezvous_local_candidate_count, payload(offset + 3, 1))
    for index = 0, candidate_count - 1 do
        local address_offset = candidates_offset + index * addr_len
        local endpoint = endpoint_text(payload, address_offset, family, local_port)
        if endpoint == nil then
            return nil, "invalid CandidatePlan local candidate"
        end
        plan:add(f.rendezvous_local_candidate, payload(address_offset, addr_len), endpoint)
    end
    local public_address = address_text(payload, public_address_offset, family)
    if public_address == nil then
        return nil, "invalid CandidatePlan public address"
    end
    plan:add(f.rendezvous_public_address, payload(public_address_offset, addr_len), public_address)
    plan:add(f.rendezvous_public_port_count, payload(port_count_offset, 1))
    for index = 0, public_port_count - 1 do
        plan:add(f.rendezvous_public_port, payload(ports_offset + index * 2, 2))
    end
    return end_offset
end

local function parse_rendezvous_payload(payload, tree, message_type, summaries)
    local payload_len = payload:len()
    local label = rendezvous_message_name(message_type)

    if message_type == 11 then
        if payload_len ~= 16 then
            return "INTRODUCTION payload must be 16 bytes"
        end
        tree:add(f.rendezvous_id, payload(0, 16))
        append_summary(summaries, "RENDEZVOUS INTRODUCTION")
        return nil
    end
    if message_type == 8 then
        if payload_len < 22 then
            return "truncated REQUEST payload"
        end
        local offset = 0
        tree:add(f.rendezvous_id, payload(offset, 16))
        offset = offset + 16
        local source_length = payload(offset, 1):uint()
        offset = offset + 1
        if source_length == 0 or source_length > 128 or offset + source_length + 1 > payload_len then
            return "invalid REQUEST source peer ID"
        end
        tree:add(f.rendezvous_source_peer_id, payload(offset, source_length), payload(offset, source_length):string())
        offset = offset + source_length
        local target_length = payload(offset, 1):uint()
        offset = offset + 1
        if target_length == 0 or target_length > 128 or offset + target_length + 5 > payload_len then
            return "invalid REQUEST target peer ID"
        end
        tree:add(f.rendezvous_target_peer_id, payload(offset, target_length), payload(offset, target_length):string())
        offset = offset + target_length
        tree:add(f.rendezvous_nat_class, payload(offset, 1))
        tree:add(f.rendezvous_family, payload(offset + 1, 1))
        tree:add(f.rendezvous_local_port, payload(offset + 2, 2))
        tree:add(f.rendezvous_local_candidate_count, payload(offset + 4, 1))
        local family = payload(offset + 1, 1):uint()
        local local_port = payload(offset + 2, 2):uint()
        local candidate_count = payload(offset + 4, 1):uint()
        local addr_len = address_length(family)
        offset = offset + 5
        if addr_len == nil or candidate_count > 4 or offset + candidate_count * addr_len ~= payload_len then
            return "invalid REQUEST local candidates"
        end
        for index = 0, candidate_count - 1 do
            local address_offset = offset + index * addr_len
            local endpoint = endpoint_text(payload, address_offset, family, local_port)
            tree:add(f.rendezvous_local_candidate, payload(address_offset, addr_len), endpoint)
        end
        append_summary(summaries, "RENDEZVOUS REQUEST")
        return nil
    end
    if message_type == 9 then
        if payload_len <= 16 then
            return "truncated REDIRECT payload"
        end
        tree:add(f.rendezvous_id, payload(0, 16))
        local next_offset, error = parse_rendezvous_candidate_plan(payload, 16, payload_len, tree)
        if next_offset == nil or next_offset ~= payload_len then
            return error or "invalid REDIRECT CandidatePlan"
        end
        append_summary(summaries, "RENDEZVOUS REDIRECT")
        return nil
    end
    if message_type == 10 then
        if payload_len <= 17 then
            return "truncated FORWARD payload"
        end
        tree:add(f.rendezvous_id, payload(0, 16))
        local source_length = payload(16, 1):uint()
        if source_length == 0 or source_length > 128 or 17 + source_length >= payload_len then
            return "invalid FORWARD source peer ID"
        end
        tree:add(f.rendezvous_source_peer_id, payload(17, source_length), payload(17, source_length):string())
        local next_offset, error = parse_rendezvous_candidate_plan(payload, 17 + source_length, payload_len, tree)
        if next_offset == nil or next_offset ~= payload_len then
            return error or "invalid FORWARD CandidatePlan"
        end
        append_summary(summaries, "RENDEZVOUS FORWARD")
        return nil
    end
    append_summary(summaries, "RENDEZVOUS " .. label)
    return nil
end

local function parse_nat_endpoint(payload, offset, length, tree, label)
    if length < 8 then
        return nil
    end
    local family = payload(offset, 1):uint()
    local addr_len = address_length(family)
    if addr_len == nil or length ~= 4 + addr_len then
        return nil
    end
    local port = payload(offset + 2, 2):uint()
    local endpoint = endpoint_text(payload, offset + 4, family, port)
    if endpoint == nil then
        return nil
    end
    local endpoint_tree = tree:add(payload(offset, length), label)
    endpoint_tree:add(f.nat_endpoint_family, payload(offset, 1))
    endpoint_tree:add(f.nat_endpoint_port, payload(offset + 2, 2))
    endpoint_tree:add(f.nat_endpoint_address, payload(offset + 4, addr_len), endpoint)
    return endpoint
end

local function parse_nat_probe(payload, tree, summaries)
    local payload_len = payload:len()
    if payload_len < 4 then
        tree:add_expert_info(PI_MALFORMED, PI_ERROR, "Truncated NAT probe header")
        return
    end
    local version = payload(0, 1):uint()
    local message_type = payload(1, 1):uint()
    local step = payload(2, 1):uint()
    local change_flags = payload(3, 1):uint()
    local header = tree:add(payload(0, 4), "NAT Probe Header")
    header:add(f.nat_version, payload(0, 1))
    header:add(f.nat_message_type, payload(1, 1))
    header:add(f.nat_step, payload(2, 1))
    header:add(f.nat_change_flags, payload(3, 1))
    local summary = string.format("NAT %s %s %s", nat_step_name(step), nat_message_name(message_type),
        nat_change_name(change_flags))
    local offset = 4
    while offset < payload_len do
        if offset + 4 > payload_len then
            tree:add_expert_info(PI_MALFORMED, PI_ERROR, "Truncated NAT probe TLV header")
            break
        end
        local tlv_type = payload(offset, 2):uint()
        local tlv_length = payload(offset + 2, 2):uint()
        local value_offset = offset + 4
        local next_offset = value_offset + tlv_length
        if next_offset > payload_len then
            tree:add_expert_info(PI_MALFORMED, PI_ERROR, "NAT probe TLV length exceeds payload")
            break
        end
        local tlv = tree:add(payload(offset, 4 + tlv_length), nat_tlv_names[tlv_type] or string.format("NAT TLV %u", tlv_type))
        tlv:add(f.nat_tlv_type, payload(offset, 2))
        tlv:add(f.nat_tlv_len, payload(offset + 2, 2))
        if tlv_length > 0 then
            tlv:add(f.nat_tlv_value, payload(value_offset, tlv_length))
        end
        if tlv_type == 1 and tlv_length == 12 then
            tlv:add(f.nat_token, payload(value_offset, tlv_length))
        elseif tlv_type == 6 or tlv_type == 7 or tlv_type == 8 then
            local endpoint = parse_nat_endpoint(payload, value_offset, tlv_length, tlv, nat_tlv_names[tlv_type])
            if endpoint ~= nil and tlv_type == 6 then
                summary = summary .. " mapped=" .. endpoint
            end
        end
        offset = next_offset
    end
    if version ~= 2 then
        tree:add_expert_info(PI_PROTOCOL, PI_WARN, string.format("Unexpected NAT probe version %u", version))
    end
    append_summary(summaries, summary)
end

local function parse_frame(payload, payload_offset, payload_len, tree, frame_index, summaries)
    if payload_offset >= payload_len then
        return 0
    end

    local frame_type = payload(payload_offset, 1):uint()
    local frame_len = 0

    if frame_type == 5 or frame_type == 15 then
        frame_len = (frame_type == 5) and 1 or 9
    elseif frame_type == 9 or frame_type == 10 then
        frame_len = 9
    elseif frame_type == 14 then
        frame_len = 5
    elseif frame_type == 3 then
        if payload_offset + 3 > payload_len then
            return -1
        end
        frame_len = 3 + payload(payload_offset + 1, 2):uint()
    elseif frame_type == 12 then
        if payload_offset + 4 > payload_len then
            return -1
        end
        frame_len = 4 + payload(payload_offset + 1, 1):uint()
    elseif frame_type == 4 then
        if payload_offset + 5 > payload_len then
            return -1
        end
        frame_len = 5 + payload(payload_offset + 3, 2):uint()
    elseif frame_type == 1 then
        if payload_offset + 16 > payload_len then
            return -1
        end
        frame_len = 16 + payload(payload_offset + 2, 2):uint()
    elseif frame_type == 2 then
        if payload_offset + 16 > payload_len then
            return -1
        end
        frame_len = 16 + payload(payload_offset + 1, 1):uint() * 8
    elseif frame_type == 11 then
        -- FRAME_CRYPTO_SIZE = 1(type) + 1(crypto_type) + 1(reserved) + 32(pubkey) = 35
        frame_len = 35
    elseif frame_type == 6 then
        frame_len = 15
    elseif frame_type == 13 then
        frame_len = 7
    elseif frame_type == 16 then
        -- FRAME_TRANSPORT_PARAMS_SIZE = 1 + 2 + 4 + 2 + 2 + 2 + 1 + 8 + 8 + 8 = 38
        frame_len = 38
    elseif frame_type == 17 then
        -- FRAME_HANDSHAKE_DELAY_SIZE = 1 + 4 = 5
        frame_len = 5
    elseif frame_type == 18 or frame_type == 20 then
        -- FRAME_MAX_DATA_SIZE / FRAME_DATA_BLOCKED_SIZE = 1 + 8 = 9
        frame_len = 9
    elseif frame_type == 19 or frame_type == 21 then
        -- FRAME_MAX_STREAM_DATA_SIZE / FRAME_STREAM_DATA_BLOCKED_SIZE = 1 + 4 + 8 = 13
        frame_len = 13
    elseif frame_type == 7 or frame_type == 8 then
        -- FRAME_STREAMS_LIMIT_SIZE = 1 + 1 + 2 = 4
        frame_len = 4
    elseif frame_type == 22 then
        -- FRAME_STOP_SENDING_SIZE = 1 + 2 + 4 = 7
        frame_len = 7
    elseif frame_type == 23 then
        if payload_offset + 4 > payload_len then
            return -1
        end
        frame_len = 4 + payload(payload_offset + 2, 2):uint()
    elseif frame_type == 24 then
        if payload_offset + 2 > payload_len then
            return -1
        end
        local family = payload(payload_offset + 1, 1):uint()
        local addr_len = address_length(family)
        if addr_len == nil then
            return -1
        end
        frame_len = 4 + addr_len
    else
        return -1
    end

    if frame_len <= 0 or (payload_offset + frame_len) > payload_len then
        return -1
    end

    local frame_tvb = payload(payload_offset, frame_len)
    local node = tree:add(frame_tvb, string.format("Frame %d: %s", frame_index, frame_name(frame_type)))
    node:add(f.frame_index, frame_index)
    node:add(f.frame_type, payload(payload_offset, 1))
    node:add(f.frame_len, frame_len)

    if frame_type == 1 then
        local base = payload_offset
        local flags = payload(base + 1, 1):uint()
        local data_len = payload(base + 2, 2):uint()
        local stream_id = payload(base + 4, 4):uint()
        local has_fin = (flags % 2) ~= 0
        node:add(f.stream_flag, payload(base + 1, 1))
        node:add(f.stream_fin, payload(base + 1, 1))
        node:add(f.stream_data_len, payload(base + 2, 2))
        node:add(f.stream_id, payload(base + 4, 4))
        node:add(f.stream_offset, payload(base + 8, 8))
        if data_len > 0 then
            node:add(f.stream_data, payload(base + 16, data_len))
        end
        append_summary(summaries, string.format("STREAM id=%u len=%u%s", stream_id, data_len, has_fin and " fin" or ""))
    elseif frame_type == 2 then
        local base = payload_offset
        local count = payload(base + 1, 1):uint()
        local first_range = payload(base + 4, 4):uint()
        local largest = payload(base + 8, 8):uint64()
        node:add(f.ack_count, payload(base + 1, 1))
        node:add(f.ack_delay, payload(base + 2, 2))
        node:add(f.ack_first_range, payload(base + 4, 4))
        node:add(f.ack_largest, payload(base + 8, 8))

        -- Absolute range for first_ack_range: [largest - first_range + 1, largest]
        if first_range > 0 then
            local first_abs_low = largest - UInt64(first_range - 1)
            local first_abs = node:add(payload(base + 4, 12), "Ack Range First (absolute)")
            first_abs:add(f.ack_abs_low, first_abs_low)
            first_abs:add(f.ack_abs_high, largest)
        end
        append_summary(summaries, string.format("ACK largest=%s ranges=%u", tostring(largest), count + 1))

        local range_off = base + 16
        local last_abs_low = nil
        if first_range > 0 then
            last_abs_low = largest - UInt64(first_range - 1)
        end
        for i = 0, count - 1 do
            local r = node:add(payload(range_off, 8), string.format("Ack Range %d", i))
            local gap = payload(range_off, 4):uint()
            local ack_len = payload(range_off + 4, 4):uint()
            r:add(f.ack_gap, payload(range_off, 4))
            r:add(f.ack_range_len, payload(range_off + 4, 4))

            if last_abs_low ~= nil and ack_len > 0 then
                -- Protocol semantics: high = prev_low - gap - 1, low = high - len + 1
                local abs_high = last_abs_low - UInt64(gap) - UInt64(1)
                local abs_low = abs_high - UInt64(ack_len - 1)
                r:add(f.ack_abs_low, abs_low)
                r:add(f.ack_abs_high, abs_high)
                last_abs_low = abs_low
            end
            range_off = range_off + 8
        end
    elseif frame_type == 3 then
        node:add(f.padding_len, payload(payload_offset + 1, 2))
        local padding_len = payload(payload_offset + 1, 2):uint()
        append_summary(summaries, string.format("PADDING len=%u", padding_len))
    elseif frame_type == 4 then
        node:add(f.close_error, payload(payload_offset + 1, 2))
        local reason_len = payload(payload_offset + 3, 2):uint()
        node:add(f.close_reason_len, payload(payload_offset + 3, 2))
        if reason_len > 0 then
            node:add(f.close_reason, payload(payload_offset + 5, reason_len):string())
        end
        append_summary(summaries, string.format("CLOSE err=%u", payload(payload_offset + 1, 2):uint()))
    elseif frame_type == 6 then
        node:add(f.reset_error, payload(payload_offset + 1, 2))
        node:add(f.reset_stream_id, payload(payload_offset + 3, 4))
        node:add(f.reset_final_size, payload(payload_offset + 7, 8))
        append_summary(summaries, string.format("RESET sid=%u err=%u", payload(payload_offset + 3, 4):uint(), payload(payload_offset + 1, 2):uint()))
    elseif frame_type == 7 or frame_type == 8 then
        node:add(f.stream_limit_type, payload(payload_offset + 1, 1))
        node:add(f.stream_limit_value, payload(payload_offset + 2, 2))
        append_summary(summaries, string.format("%s type=%u limit=%u", frame_type == 7 and "STREAMS_BLOCKED" or "MAX_STREAMS",
            payload(payload_offset + 1, 1):uint(), payload(payload_offset + 2, 2):uint()))
    elseif frame_type == 9 or frame_type == 10 then
        node:add(f.path_data, payload(payload_offset + 1, 8))
        append_summary(summaries, frame_type == 9 and "PATH_CHALLENGE" or "PATH_RESPONSE")
    elseif frame_type == 11 then
        node:add(f.crypto_type, payload(payload_offset + 1, 1))
        node:add(f.crypto_reserved, payload(payload_offset + 2, 1))
        node:add(f.crypto_pubkey, payload(payload_offset + 3, 32))
        append_summary(summaries, string.format("CRYPTO type=%u", payload(payload_offset + 1, 1):uint()))
    elseif frame_type == 12 then
        local token_size = payload(payload_offset + 1, 1):uint()
        node:add(f.token_size, payload(payload_offset + 1, 1))
        node:add(f.token_validity, payload(payload_offset + 2, 2))
        if token_size > 0 then
            node:add(f.token_data, payload(payload_offset + 4, token_size))
        end
        append_summary(summaries, string.format("TOKEN len=%u", token_size))
    elseif frame_type == 13 then
        node:add(f.ack_freq_thresh, payload(payload_offset + 1, 1))
        node:add(f.ack_freq_reorder, payload(payload_offset + 2, 1))
        node:add(f.ack_freq_max_delay, payload(payload_offset + 3, 4))
        append_summary(summaries, string.format("ACK_FREQUENCY n=%u", payload(payload_offset + 1, 1):uint()))
    elseif frame_type == 14 then
        node:add(f.version, payload(payload_offset + 1, 4))
        append_summary(summaries, string.format("VERSION %u", payload(payload_offset + 1, 4):uint()))
    elseif frame_type == 16 then
        node:add(f.tp_flags, payload(payload_offset + 1, 2))
        node:add(f.tp_max_idle_timeout, payload(payload_offset + 3, 4))
        node:add(f.tp_handshake_timeout, payload(payload_offset + 7, 2))
        node:add(f.tp_init_max_streams_bidi, payload(payload_offset + 9, 2))
        node:add(f.tp_init_max_streams_uni, payload(payload_offset + 11, 2))
        node:add(f.tp_ack_delay_exponent, payload(payload_offset + 13, 1))
        node:add(f.tp_initial_max_data, payload(payload_offset + 14, 8))
        node:add(f.tp_initial_max_stream_data_bidi_local, payload(payload_offset + 22, 8))
        node:add(f.tp_initial_max_stream_data_bidi_remote, payload(payload_offset + 30, 8))
        append_summary(summaries, string.format("TP hs_to=%u max_data=%s",
            payload(payload_offset + 7, 2):uint(),
            tostring(payload(payload_offset + 14, 8):uint64())))
    elseif frame_type == 5 then
        append_summary(summaries, "PING")
    elseif frame_type == 15 then
        node:add(f.handshake_done_ack_pn, payload(payload_offset + 1, 8))
        append_summary(summaries, string.format("HANDSHAKE_DONE ack_pn=%s", tostring(payload(payload_offset + 1, 8):uint64())))
    elseif frame_type == 17 then
        node:add(f.handshake_delay_us, payload(payload_offset + 1, 4))
        append_summary(summaries, string.format("HANDSHAKE_DELAY us=%u", payload(payload_offset + 1, 4):uint()))
    elseif frame_type == 18 then
        node:add(f.max_data_limit, payload(payload_offset + 1, 8))
        append_summary(summaries, string.format("MAX_DATA %s", tostring(payload(payload_offset + 1, 8):uint64())))
    elseif frame_type == 19 then
        node:add(f.max_stream_data_stream_id, payload(payload_offset + 1, 4))
        node:add(f.max_stream_data_limit, payload(payload_offset + 5, 8))
        append_summary(summaries, string.format("MAX_STREAM_DATA sid=%u limit=%s",
            payload(payload_offset + 1, 4):uint(),
            tostring(payload(payload_offset + 5, 8):uint64())))
    elseif frame_type == 20 then
        node:add(f.data_blocked_limit, payload(payload_offset + 1, 8))
        append_summary(summaries, string.format("DATA_BLOCKED %s", tostring(payload(payload_offset + 1, 8):uint64())))
    elseif frame_type == 21 then
        node:add(f.stream_data_blocked_stream_id, payload(payload_offset + 1, 4))
        node:add(f.stream_data_blocked_limit, payload(payload_offset + 5, 8))
        append_summary(summaries, string.format("STREAM_DATA_BLOCKED sid=%u limit=%s",
            payload(payload_offset + 1, 4):uint(),
            tostring(payload(payload_offset + 5, 8):uint64())))
    elseif frame_type == 22 then
        node:add(f.stop_sending_error, payload(payload_offset + 1, 2))
        node:add(f.stop_sending_stream_id, payload(payload_offset + 3, 4))
        append_summary(summaries, string.format("STOP_SENDING sid=%u err=%u", payload(payload_offset + 3, 4):uint(),
            payload(payload_offset + 1, 2):uint()))
    elseif frame_type == 23 then
        local message_type = payload(payload_offset + 1, 1):uint()
        local body_len = payload(payload_offset + 2, 2):uint()
        local body = payload(payload_offset + 4, body_len)
        local rendezvous = node:add(frame_tvb, "Rendezvous: " .. rendezvous_message_name(message_type))
        rendezvous:add(f.rendezvous_message_type, payload(payload_offset + 1, 1))
        rendezvous:add(f.rendezvous_payload_len, payload(payload_offset + 2, 2))
        if body_len > 0 then
            rendezvous:add(f.rendezvous_payload, body)
        end
        local error = parse_rendezvous_payload(body, rendezvous, message_type, summaries)
        if error ~= nil then
            rendezvous:add_expert_info(PI_MALFORMED, PI_ERROR, error)
        end
    elseif frame_type == 24 then
        local family = payload(payload_offset + 1, 1):uint()
        local addr_len = address_length(family)
        local port = payload(payload_offset + 2, 2):uint()
        local address = address_text(payload, payload_offset + 4, family)
        node:add(f.observed_family, payload(payload_offset + 1, 1))
        node:add(f.observed_port, payload(payload_offset + 2, 2))
        node:add(f.observed_address, payload(payload_offset + 4, addr_len), address)
        append_summary(summaries, string.format("OBSERVED_ADDRESS %s", endpoint_text(payload, payload_offset + 4, family, port)))
    end

    return frame_len
end

function utp.dissector(buffer, pinfo, tree)
    local total_len = buffer:len()
    if total_len < 20 then
        return 0
    end

    local packet_type = buffer(18, 1):uint()
    local payload_len = buffer(16, 2):uint()
    local utp_packet_len = 20 + payload_len

    if packet_type_names[packet_type] == nil then
        return 0
    end
    if utp_packet_len > total_len then
        return 0
    end

    local subtree = tree:add(utp, buffer(), "Eular UTP Protocol")
    subtree:add(f.scid, buffer(0, 4))
    subtree:add(f.dcid, buffer(4, 4))
    subtree:add(f.pn, buffer(8, 8))
    subtree:add(f.payload_len, buffer(16, 2))
    subtree:add(f.packet_len, utp_packet_len)
    subtree:add(f.packet_type, buffer(18, 1))
    subtree:add(f.reserve, buffer(19, 1))

    local payload = buffer(20, payload_len)
    local payload_tree = subtree:add(payload, packet_type == 0x07 and "NAT Probe Payload" or "Payload Frames")
    local off = 0
    local frame_index = 0
    local maybe_encrypted = packet_can_be_encrypted(packet_type)
    local frame_summaries = {}
    if packet_type == 0x07 then
        parse_nat_probe(payload, payload_tree, frame_summaries)
    else
        while off < payload_len do
            local consumed = parse_frame(payload, off, payload_len, payload_tree, frame_index, frame_summaries)
            if consumed <= 0 then
                if maybe_encrypted then
                    if off == 0 then
                        payload_tree:add_expert_info(PI_PROTOCOL, PI_NOTE,
                            "Payload likely encrypted; frame parsing skipped")
                        append_summary(frame_summaries, "ENCRYPTED")
                        if payload_len > 16 then
                            payload_tree:add(f.payload_cipher, payload(0, payload_len - 16))
                            payload_tree:add(f.payload_tag, payload(payload_len - 16, 16))
                        else
                            payload_tree:add(f.payload_raw, payload)
                        end
                    else
                        payload_tree:add_expert_info(PI_PROTOCOL, PI_NOTE,
                            string.format("Frame parsing stopped at payload offset %d; remaining bytes undecoded", off))
                        payload_tree:add(f.payload_undecoded, payload(off, payload_len - off))
                    end
                else
                    payload_tree:add_expert_info(PI_MALFORMED, PI_ERROR,
                        string.format("Malformed frame at payload offset %d", off))
                end
                break
            end
            off = off + consumed
            frame_index = frame_index + 1
        end
    end

    subtree:add(f.frame_count, frame_index)

    pinfo.cols.protocol = "UTP"
    local info = string.format("%s pn=%s scid=0x%08x dcid=0x%08x payload=%u frames=%u",
        packet_type_name(packet_type),
        tostring(buffer(8, 8):uint64()),
        buffer(0, 4):uint(),
        buffer(4, 4):uint(),
        payload_len,
        frame_index)
    if #frame_summaries > 0 then
        info = info .. " " .. table.concat(frame_summaries, ", ")
    end
    pinfo.cols.info = info

    return total_len
end

local udp_port_table = DissectorTable.get("udp.port")
local current_udp_port = 0

local function bind_port(port)
    if current_udp_port ~= 0 then
        udp_port_table:remove(current_udp_port, utp)
    end
    current_udp_port = port
    if current_udp_port ~= 0 then
        udp_port_table:add(current_udp_port, utp)
    end
end

function utp.init()
    bind_port(utp.prefs.udp_port)
end

function utp.prefs_changed()
    bind_port(utp.prefs.udp_port)
end

bind_port(utp.prefs.udp_port)
