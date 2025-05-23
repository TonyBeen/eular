-- Proto 注册新协议
kcp_proto = Proto("KCP", "KCP Protocol");

local kcp_payload_type = {
    [0x01] = "SYN",  [0x02] = "ACK",  [0x03] = "PUSH", [0x04] = "WASK",
    [0x05] = "WINS", [0x06] = "PING", [0x07] = "PONG", [0x08] = "MTU Probe", [0x09] = "MTU Ack",
    [0x0a] = "FIN",  [0x0b] = "RST"
}

local n_kcp_header_size = 32

local s_conversation    = ProtoField.uint32("kcp.conversation", "Conversation", base.HEX)
local s_command         = ProtoField.uint8("kcp.command", "Command", base.HEX, kcp_payload_type)
local s_fragmentation   = ProtoField.uint8("kcp.fragmentation", "Fragmentation", base.DEC)
local s_window          = ProtoField.uint16("kcp.window", "Window", base.DEC)

local s_payload_value   = ProtoField.string("kcp.payload", "Payload Info")

-- packet info
local s_packet_ts       = ProtoField.uint64("kcp.packet.ts", "TS", base.DEC)
local s_packet_sn       = ProtoField.uint32("kcp.packet.sn", "SN", base.DEC)
local s_packet_psn      = ProtoField.uint32("kcp.packet.psn", "Packet SN", base.DEC)
local s_packet_una      = ProtoField.uint32("kcp.packet.una", "UNA", base.DEC)
local s_packet_len      = ProtoField.uint32("kcp.packet.len", "Len", base.DEC)
local s_packet_data     = ProtoField.bytes("kcp.packet.data", "Data")

-- syn fin info
local s_syn_packet_ts   = ProtoField.uint64("kcp.syn.packet_ts", "Packet TS", base.DEC)
local s_syn_ts          = ProtoField.uint64("kcp.syn.ts", "TS", base.DEC)
local s_syn_packet_sn   = ProtoField.uint32("kcp.syn.packet_sn", "Packet SN", base.DEC)
local s_syn_rand_sn     = ProtoField.uint32("kcp.syn.rand_sn", "Rand SN", base.DEC)

-- ack info
local s_ack_packet_ts   = ProtoField.uint64("kcp.ack.packet_ts", "Packet TS", base.DEC)
local s_ack_ts          = ProtoField.uint64("kcp.ack.ts", "Ack TS", base.DEC)
local s_ack_sn          = ProtoField.uint32("kcp.ack.sn", "SN", base.DEC)
local s_ack_una         = ProtoField.uint32("kcp.ack.una", "UNA", base.DEC)

-- ping info
local s_ping_packet_ts  = ProtoField.uint64("kcp.ping.packet_ts", "Packet TS", base.DEC)
local s_ping_ts         = ProtoField.uint64("kcp.ping.ts", "TS", base.DEC)
local s_ping_sn         = ProtoField.uint64("kcp.ping.sn", "SN", base.DEC)

kcp_proto.fields = {
    s_conversation, s_command, s_fragmentation, s_window,
    s_payload_value,
    s_packet_ts, s_packet_sn, s_packet_psn, s_packet_una, s_packet_len, s_packet_data,
    s_syn_packet_ts, s_syn_ts, s_syn_packet_sn, s_syn_rand_sn,
    s_ack_packet_ts, s_ack_ts, s_ack_sn, s_ack_una,
    s_ping_packet_ts, s_ping_ts, s_ping_sn
}

function ParsePacket(tvb, offset, length, pinfo, tree, cmd_string)
    -- ts
    local ts = tvb(offset, 8)
    tree:add_le(s_packet_ts, ts)
    offset = offset + 8

    -- sn
    local sn = tvb(offset, 4)
    local sn_value = tvb(offset, 4):le_uint()
    tree:add_le(s_packet_sn, sn)
    offset = offset + 4

    -- psn
    local psn = tvb(offset, 4)
    local psn_value = tvb(offset, 4):le_uint()
    tree:add_le(s_packet_psn, psn)
    offset = offset + 4

    -- una
    local una = tvb(offset, 4)
    tree:add_le(s_packet_una, una)
    offset = offset + 4

    -- len
    local len = tvb(offset, 4)
    local n_length = tvb(offset, 4):le_uint()
    tree:add_le(s_packet_len, len)
    offset = offset + 4

    if n_length > 0 then
        -- data 8 表示 conv + cmd + frg + window 占用字节数
        local data = tvb(offset, length - n_kcp_header_size + 8)
        tree:add(s_packet_data, data)
        offset = offset + n_length
    end

    local append_info = cmd_string .. " SN=" .. sn_value .. ", PSN=" .. psn_value .. ", Len=" .. n_length
    pinfo.cols.info:append(append_info)

    return offset
end

function ParseAck(tvb, offset, length, pinfo, tree, cmd_string)
    -- packet ts
    local packet_ts = tvb(offset, 8)
    tree:add_le(s_ack_packet_ts, packet_ts)
    offset = offset + 8

    -- ts
    local ts = tvb(offset, 8)
    tree:add_le(s_ack_ts, ts)
    offset = offset + 8

    -- sn
    local sn = tvb(offset, 4)
    local sn_value = tvb(offset, 4):le_uint()
    tree:add_le(s_ack_sn, sn)
    offset = offset + 4

    -- una
    local una = tvb(offset, 4)
    local una_value = tvb(offset, 4):le_uint()
    tree:add_le(s_ack_una, una)
    offset = offset + 4

    local append_info = cmd_string .. " SN=" .. sn_value .. ", UNA=" .. una_value .. "; "
    pinfo.cols.info:append(append_info)

    return offset
end

function ParsePing(tvb, offset, length, pinfo, tree, cmd_string)
    -- packet ts
    local packet_ts = tvb(offset, 8)
    tree:add_le(s_ping_packet_ts, packet_ts)
    offset = offset + 8

    -- ts
    local ts = tvb(offset, 8)
    tree:add_le(s_ping_ts, ts)
    offset = offset + 8

    -- sn
    local sn = tvb(offset, 8)
    local sn_value = tvb(offset, 8):le_uint64()
    tree:add_le(s_ping_sn, sn)
    offset = offset + 8

    local append_info = cmd_string .. " SN=" .. sn_value .. "; "
    pinfo.cols.info:append(append_info)

    return offset
end

function ParseSyn(tvb, offset, length, pinfo, tree, cmd_string)
    -- packet ts
    local packet_ts = tvb(offset, 8)
    tree:add_le(s_syn_packet_ts, packet_ts)
    offset = offset + 8

    -- ts
    local ts = tvb(offset, 8)
    tree:add_le(s_syn_ts, ts)
    offset = offset + 8

    -- packet sn
    local sn = tvb(offset, 4)
    local packet_sn_value = tvb(offset, 4):le_uint()
    tree:add_le(s_syn_packet_sn, sn)
    offset = offset + 4

    -- rand sn
    local rand_sn = tvb(offset, 4)
    local rand_sn_value = tvb(offset, 4):le_uint()
    tree:add_le(s_syn_rand_sn, rand_sn)
    offset = offset + 4

    local append_info = cmd_string .. " Packet SN=" .. packet_sn_value .. ", Rand SN=" .. rand_sn_value
    pinfo.cols.info:append(append_info)

    return offset
end

function kcp_proto.dissector(buf, pkt, root)
    local length = buf:len()
    if length == 0 then
        return
    end

    pkt.cols.protocol = kcp_proto.name
    pkt.cols.info = ""

    local src_port = pkt.src_port
    local dst_port = pkt.dst_port

    pkt.cols.info = tostring(src_port) .. " → " .. tostring(dst_port)

    local total_len = buf:len()
    local size_offset = 0
    local remaining_len = total_len

    while (true)
    do
        -- 校验头部长度
        if remaining_len < n_kcp_header_size then
            if (remaining_len > 0) then
                pkt.cols.info = tostring(pkt.cols.info) .. "; Error!!! remaining_len = " .. remaining_len .. " < " .. n_kcp_header_size
            end
            return
        end

        local kcp_tree = root:add(kcp_proto, buf(), "KCP Protocol")

        local offset = size_offset
        local parse_size = 0
        -- conversation
        local conversation_value = buf(offset, 4):le_uint()
        kcp_tree:add_le(s_conversation, buf(offset, 4))
        offset = offset + 4

        -- cmd
        kcp_tree:add(s_command, buf(offset, 1))
        local command = buf(offset, 1):le_uint()
        offset = offset + 1

        -- frg
        kcp_tree:add(s_fragmentation, buf(offset, 1))
        offset = offset + 1

        -- window
        kcp_tree:add_le(s_window, buf(offset, 2))
        offset = offset + 2

        -- info
        local cmd_string = kcp_payload_type[command]
        if not cmd_string then
            cmd_string = string.format("UNKNOWN(0x%02X)", command)
        end
        cmd_string = string.format(" [%s] Conv=0x%04X,", cmd_string, conversation_value)

        local payload_len = total_len - offset

        local payload_value_tree = kcp_tree:add(s_payload_value, buf(offset, payload_len), "")
        if command == 0x01 or command == 0x0a then -- SYN/FIN
            parse_size = ParseSyn(buf, offset, payload_len, pkt, payload_value_tree, cmd_string)
        elseif command == 0x02 then -- ACK
            parse_size = ParseAck(buf, offset, payload_len, pkt, payload_value_tree, cmd_string)
        elseif command == 0x06 or command == 0x07 then -- PING/PONG
            parse_size = ParsePing(buf, offset, payload_len, pkt, payload_value_tree, cmd_string)
        else -- push, wask, wins, mtu probe, mtu ack, rst
            parse_size = ParsePacket(buf, offset, payload_len, pkt, payload_value_tree, cmd_string)
        end

        size_offset = parse_size
        remaining_len = total_len - size_offset
        if remaining_len <= 0 then
            break
        end
    end
end

-- kcp 默认端口
local udp_table = DissectorTable.get("udp.port")
for i = 54321, 65432 do
    udp_table:add(i, kcp_proto)
end
