-- Eular UTP Wireshark Lua dissector
-- Protocol layout comes from src/proto/proto.h and src/proto/frame/*.h

local utp = Proto("UTP", "Eular UTP")

local packet_type_names = {
    [0x00] = "NONE",
    [0x01] = "INITIAL",
    [0x02] = "HANDSHAKE",
    [0x03] = "0RTT",
    [0x04] = "CONNECTION_CLOSE",
    [0x05] = "CTRL",
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

f.reset_error = ProtoField.uint16("eular_utp.reset.error", "Reset Error Code", base.DEC)
f.reset_stream_id = ProtoField.uint32("eular_utp.reset.stream_id", "Reset Stream ID", base.DEC)
f.reset_final_size = ProtoField.uint64("eular_utp.reset.final_size", "Reset Final Size", base.DEC)

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

local function append_summary(summaries, text)
    if text == nil or text == "" then
        return
    end
    if #summaries >= 4 then
        return
    end
    summaries[#summaries + 1] = text
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
        -- FRAME_TRANSPORT_PARAMS_SIZE = 1 + 2 + 4 + 2 + 2 + 2 + 1 = 14
        frame_len = 14
    elseif frame_type == 17 then
        -- FRAME_HANDSHAKE_DELAY_SIZE = 1 + 4 = 5
        frame_len = 5
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
        append_summary(summaries, string.format("TP hs_to=%u", payload(payload_offset + 7, 2):uint()))
    elseif frame_type == 5 then
        append_summary(summaries, "PING")
    elseif frame_type == 15 then
        node:add(f.handshake_done_ack_pn, payload(payload_offset + 1, 8))
        append_summary(summaries, string.format("HANDSHAKE_DONE ack_pn=%s", tostring(payload(payload_offset + 1, 8):uint64())))
    elseif frame_type == 17 then
        node:add(f.handshake_delay_us, payload(payload_offset + 1, 4))
        append_summary(summaries, string.format("HANDSHAKE_DELAY us=%u", payload(payload_offset + 1, 4):uint()))
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

    local payload_tree = subtree:add(buffer(20, payload_len), "Payload Frames")
    local off = 0
    local frame_index = 0
    local maybe_encrypted = packet_can_be_encrypted(packet_type)
    local frame_summaries = {}
    while off < payload_len do
        local consumed = parse_frame(buffer(20, payload_len), off, payload_len, payload_tree, frame_index, frame_summaries)
        if consumed <= 0 then
            local payload_tvb = buffer(20, payload_len)
            if maybe_encrypted then
                if off == 0 then
                    payload_tree:add_expert_info(PI_PROTOCOL, PI_NOTE,
                        "Payload likely encrypted; frame parsing skipped")
                    append_summary(frame_summaries, "ENCRYPTED")
                    if payload_len > 16 then
                        payload_tree:add(f.payload_cipher, payload_tvb(0, payload_len - 16))
                        payload_tree:add(f.payload_tag, payload_tvb(payload_len - 16, 16))
                    else
                        payload_tree:add(f.payload_raw, payload_tvb)
                    end
                else
                    payload_tree:add_expert_info(PI_PROTOCOL, PI_NOTE,
                        string.format("Frame parsing stopped at payload offset %d; remaining bytes undecoded", off))
                    payload_tree:add(f.payload_undecoded, payload_tvb(off, payload_len - off))
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
