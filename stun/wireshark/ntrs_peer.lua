local stun_peer_proto = Proto("stun_peer", "STUN Peer Probe")

local f_msg_type = ProtoField.string("stun_peer.type", "Type")
local f_candidate_type = ProtoField.string("stun_peer.candidate_type", "Candidate Type")
local f_round = ProtoField.uint32("stun_peer.round", "Round", base.DEC)
local f_owner_peer = ProtoField.string("stun_peer.owner_peer", "Owner Peer")
local f_token = ProtoField.string("stun_peer.token", "Token")
local f_seq = ProtoField.uint32("stun_peer.seq", "Sequence", base.DEC)
local f_value = ProtoField.string("stun_peer.value", "Value")
local f_payload_len = ProtoField.uint32("stun_peer.payload_len", "Payload Length", base.DEC)
local f_raw = ProtoField.string("stun_peer.raw", "Raw")

stun_peer_proto.fields = {
    f_msg_type,
    f_candidate_type,
    f_round,
    f_owner_peer,
    f_token,
    f_seq,
    f_value,
    f_payload_len,
    f_raw,
}

local function split_fields(s)
    local fields = {}
    for part in string.gmatch(s, "([^|]+)") do
        table.insert(fields, part)
    end
    return fields
end

local function looks_like_stun_peer(payload)
    return payload:find("^STUN_PUNCH_REQ|") ~= nil
        or payload:find("^STUN_PUNCH_ACK|") ~= nil
        or payload:find("^STUN_PROBE_PING|") ~= nil
        or payload:find("^STUN_PROBE_PONG|") ~= nil
        or payload:find("^STUN_MTU_PROBE|") ~= nil
        or payload:find("^STUN_MTU_ACK|") ~= nil
end

function stun_peer_proto.dissector(buf, pinfo, tree)
    local length = buf:len()
    if length == 0 then
        return 0
    end

    local payload = buf(0, length):string()
    if not looks_like_stun_peer(payload) then
        return 0
    end

    pinfo.cols.protocol = "STUN_PEER"

    local subtree = tree:add(stun_peer_proto, buf(), "STUN Peer Probe")
    subtree:add(f_payload_len, buf(0, 0), length)
    subtree:add(f_raw, payload)

    local fields = split_fields(payload)
    local msg_type = fields[1] or "UNKNOWN"
    subtree:add(f_msg_type, buf(0, 0), msg_type)

    if msg_type == "STUN_PUNCH_REQ" or msg_type == "STUN_PUNCH_ACK" then
        if fields[2] ~= nil then
            subtree:add(f_candidate_type, buf(0, 0), fields[2])
        end
        if fields[3] ~= nil then
            subtree:add(f_round, buf(0, 0), tonumber(fields[3]) or 0)
        end
        pinfo.cols.info = string.format("%s candidate=%s round=%s", msg_type, fields[2] or "-", fields[3] or "-")
        return length
    end

    if #fields >= 5 then
        subtree:add(f_owner_peer, buf(0, 0), fields[2] or "")
        subtree:add(f_token, buf(0, 0), fields[3] or "")
        subtree:add(f_seq, buf(0, 0), tonumber(fields[4]) or 0)
        subtree:add(f_value, buf(0, 0), fields[5] or "")
        pinfo.cols.info = string.format("%s owner=%s token=%s seq=%s value=%s",
            msg_type, fields[2] or "-", fields[3] or "-", fields[4] or "-", fields[5] or "-")
        return length
    end

    pinfo.cols.info = msg_type
    return length
end

local udp_table = DissectorTable.get("udp.port")
udp_table:add_for_decode_as(stun_peer_proto)
