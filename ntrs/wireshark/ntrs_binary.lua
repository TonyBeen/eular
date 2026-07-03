local ntrs_proto = Proto("ntrs_binary", "NTRS Binary Probe")

local FRAME_MAGIC = 0x4E545250
local FRAME_VERSION = 1
local FRAME_HEADER_LEN = 24
local TLV_HEADER_LEN = 4

local frame_type_names = {
    [1] = "PROBE_REQ",
    [2] = "PROBE_RSP",
    [3] = "FILTER_REQ",
    [4] = "FILTER_RSP",
    [5] = "PUNCH_REQ",
    [6] = "PUNCH_ACK",
}

local phase_names = {
    [1] = "PROBE1",
    [2] = "CHANGE_PORT",
    [3] = "CHANGE_IP",
    [4] = "PROBE2",
    [5] = "PUNCH",
}

local tlv_type_names = {
    [1] = "PROBE_TOKEN",
    [2] = "PEER_ID",
    [3] = "DEVICE_ID",
    [4] = "SESSION_ID",
    [5] = "TARGET_NODE_ID",
    [6] = "MAPPED_ADDR",
    [7] = "ORIGIN_ADDR",
    [8] = "OTHER_ADDR",
    [9] = "AUTH_TAG",
    [10] = "REASON_CODE",
    [11] = "CANDIDATE_TYPE",
}

local f_magic = ProtoField.uint32("ntrs_binary.magic", "Magic", base.HEX)
local f_version = ProtoField.uint8("ntrs_binary.version", "Version", base.DEC)
local f_frame_type = ProtoField.uint8("ntrs_binary.frame_type", "Frame Type", base.DEC, frame_type_names)
local f_phase = ProtoField.uint8("ntrs_binary.phase", "Phase", base.DEC, phase_names)
local f_flags = ProtoField.uint8("ntrs_binary.flags", "Flags", base.HEX)
local f_request_id = ProtoField.uint32("ntrs_binary.request_id", "Request ID", base.DEC)
local f_sequence = ProtoField.uint32("ntrs_binary.sequence", "Sequence", base.DEC)
local f_timestamp_ms = ProtoField.uint64("ntrs_binary.timestamp_ms", "Timestamp (ms)", base.DEC)
local f_payload_len = ProtoField.uint32("ntrs_binary.payload_len", "Payload Length", base.DEC)

local f_tlv_type = ProtoField.uint16("ntrs_binary.tlv.type", "TLV Type", base.DEC, tlv_type_names)
local f_tlv_len = ProtoField.uint16("ntrs_binary.tlv.length", "TLV Length", base.DEC)
local f_tlv_value = ProtoField.bytes("ntrs_binary.tlv.value", "TLV Value")
local f_tlv_text = ProtoField.string("ntrs_binary.tlv.text", "Text")
local f_tlv_hex = ProtoField.string("ntrs_binary.tlv.hex", "Hex")
local f_addr_family = ProtoField.uint8("ntrs_binary.addr.family", "Address Family", base.DEC, {
    [4] = "IPv4",
    [6] = "IPv6",
})
local f_addr_port = ProtoField.uint16("ntrs_binary.addr.port", "Port", base.DEC)
local f_addr_ip = ProtoField.string("ntrs_binary.addr.ip", "IP")

ntrs_proto.fields = {
    f_magic,
    f_version,
    f_frame_type,
    f_phase,
    f_flags,
    f_request_id,
    f_sequence,
    f_timestamp_ms,
    f_payload_len,
    f_tlv_type,
    f_tlv_len,
    f_tlv_value,
    f_tlv_text,
    f_tlv_hex,
    f_addr_family,
    f_addr_port,
    f_addr_ip,
}

local function bytes_to_hex(range)
    local ba = range:bytes()
    if ba == nil then
        return ""
    end
    return tostring(ba):gsub(":", "")
end

local function parse_endpoint_string(value_range)
    local len = value_range:len()
    if len ~= 8 and len ~= 20 then
        return nil
    end

    local family = value_range(0, 1):uint()
    local port = value_range(2, 2):uint()
    local ip = nil

    if family == 4 and len == 8 then
        ip = tostring(value_range(4, 4):ipv4())
    elseif family == 6 and len == 20 then
        ip = tostring(value_range(4, 16):ipv6())
    else
        return nil
    end

    return {
        family = family,
        port = port,
        ip = ip,
    }
end

local function add_tlv_detail(tlv_tree, tlv_type, value_range)
    local endpoint = parse_endpoint_string(value_range)
    if endpoint ~= nil then
        tlv_tree:add(f_addr_family, value_range(0, 1))
        tlv_tree:add(f_addr_port, value_range(2, 2))
        tlv_tree:add(f_addr_ip, endpoint.ip)
        return endpoint.ip .. ":" .. tostring(endpoint.port)
    end

    if tlv_type == 1 or tlv_type == 9 then
        local hex_text = bytes_to_hex(value_range)
        tlv_tree:add(f_tlv_hex, hex_text)
        return hex_text
    end

    if tlv_type == 10 then
        if value_range:len() == 1 then
            local code = value_range(0, 1):uint()
            tlv_tree:add(f_tlv_text, tostring(code))
            return tostring(code)
        end
        tlv_tree:add(f_tlv_value, value_range)
        return nil
    end

    local ok, text = pcall(function()
        return value_range:string()
    end)
    if ok and text ~= nil and text ~= "" then
        tlv_tree:add(f_tlv_text, text)
        return text
    end

    tlv_tree:add(f_tlv_value, value_range)
    return nil
end

local function looks_like_ntrs_binary(buf)
    if buf:len() < FRAME_HEADER_LEN then
        return false
    end

    if buf(0, 4):uint() ~= FRAME_MAGIC then
        return false
    end

    if buf(4, 1):uint() ~= FRAME_VERSION then
        return false
    end

    local frame_type = buf(5, 1):uint()
    local phase = buf(6, 1):uint()
    if frame_type_names[frame_type] == nil then
        return false
    end
    if phase_names[phase] == nil then
        return false
    end

    return true
end

function ntrs_proto.dissector(buf, pinfo, tree)
    if not looks_like_ntrs_binary(buf) then
        return 0
    end

    pinfo.cols.protocol = "NTRS"

    local length = buf:len()
    local payload_len = length - FRAME_HEADER_LEN
    local frame_type = buf(5, 1):uint()
    local phase = buf(6, 1):uint()
    local request_id = buf(8, 4):uint()
    local sequence = buf(12, 4):uint()

    local subtree = tree:add(ntrs_proto, buf(), "NTRS Binary Probe")
    subtree:add(f_magic, buf(0, 4))
    subtree:add(f_version, buf(4, 1))
    subtree:add(f_frame_type, buf(5, 1))
    subtree:add(f_phase, buf(6, 1))
    subtree:add(f_flags, buf(7, 1))
    subtree:add(f_request_id, buf(8, 4))
    subtree:add(f_sequence, buf(12, 4))
    subtree:add(f_timestamp_ms, buf(16, 8))
    subtree:add(f_payload_len, payload_len)

    pinfo.cols.info = string.format("%s phase=%s req=%u seq=%u",
        frame_type_names[frame_type] or tostring(frame_type),
        phase_names[phase] or tostring(phase),
        request_id,
        sequence)

    local offset = FRAME_HEADER_LEN
    while offset + TLV_HEADER_LEN <= length do
        local tlv_type = buf(offset, 2):uint()
        local tlv_len = buf(offset + 2, 2):uint()
        local value_offset = offset + TLV_HEADER_LEN
        local end_offset = value_offset + tlv_len

        if end_offset > length then
            local bad_tree = subtree:add(ntrs_proto, buf(offset, length - offset), "Malformed TLV")
            bad_tree:add(f_tlv_type, buf(offset, 2))
            bad_tree:add(f_tlv_len, buf(offset + 2, 2))
            break
        end

        local label = tlv_type_names[tlv_type] or ("TLV_" .. tostring(tlv_type))
        local tlv_tree = subtree:add(ntrs_proto, buf(offset, TLV_HEADER_LEN + tlv_len), "TLV: " .. label)
        tlv_tree:add(f_tlv_type, buf(offset, 2))
        tlv_tree:add(f_tlv_len, buf(offset + 2, 2))
        local summary = add_tlv_detail(tlv_tree, tlv_type, buf(value_offset, tlv_len))
        if summary == nil then
            tlv_tree:add(f_tlv_value, buf(value_offset, tlv_len))
        end

        offset = end_offset
    end

    return length
end

local udp_table = DissectorTable.get("udp.port")
udp_table:add_for_decode_as(ntrs_proto)
