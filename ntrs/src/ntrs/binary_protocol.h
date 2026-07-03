#ifndef __NTRS_BINARY_PROTOCOL_H__
#define __NTRS_BINARY_PROTOCOL_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief NTRS 二进制协议魔数。
 */
#define NTRS_BINARY_FRAME_MAGIC 0x4E545250u

/**
 * @brief NTRS 二进制协议版本号。
 */
#define NTRS_BINARY_FRAME_VERSION 1u

/**
 * @brief NTRS 单帧允许的默认最大长度。
 */
#define NTRS_BINARY_FRAME_MAX_SIZE 2048u

/**
 * @brief NTRS 二进制帧类型。
 *
 * 所有私有 NAT 探测与打洞报文必须使用二进制帧类型，不允许再通过文本前缀
 * 区分消息语义。字段值本身可以承载文本，例如 `peer_id`，但 framing 与 TLV
 * 元数据必须全部为二进制。
 */
typedef enum ntrs_binary_frame_type {
    NTRS_BINARY_FRAME_UNKNOWN = 0,
    NTRS_BINARY_FRAME_PROBE_REQ = 1,
    NTRS_BINARY_FRAME_PROBE_RSP = 2,
    NTRS_BINARY_FRAME_FILTER_REQ = 3,
    NTRS_BINARY_FRAME_FILTER_RSP = 4,
    NTRS_BINARY_FRAME_PUNCH_REQ = 5,
    NTRS_BINARY_FRAME_PUNCH_ACK = 6,
} ntrs_binary_frame_type_t;

/**
 * @brief NTRS 二进制协议探测阶段。
 */
typedef enum ntrs_binary_phase {
    NTRS_BINARY_PHASE_UNKNOWN = 0,
    NTRS_BINARY_PHASE_PROBE1 = 1,
    NTRS_BINARY_PHASE_CHANGE_PORT = 2,
    NTRS_BINARY_PHASE_CHANGE_IP = 3,
    NTRS_BINARY_PHASE_PROBE2 = 4,
    NTRS_BINARY_PHASE_PUNCH = 5,
} ntrs_binary_phase_t;

/**
 * @brief NTRS 二进制 TLV 字段类型。
 */
typedef enum ntrs_binary_tlv_type {
    NTRS_BINARY_TLV_UNKNOWN = 0,
    NTRS_BINARY_TLV_PROBE_TOKEN = 1,
    NTRS_BINARY_TLV_PEER_ID = 2,
    NTRS_BINARY_TLV_DEVICE_ID = 3,
    NTRS_BINARY_TLV_SESSION_ID = 4,
    NTRS_BINARY_TLV_TARGET_NODE_ID = 5,
    NTRS_BINARY_TLV_MAPPED_ADDR = 6,
    NTRS_BINARY_TLV_ORIGIN_ADDR = 7,
    NTRS_BINARY_TLV_OTHER_ADDR = 8,
    NTRS_BINARY_TLV_AUTH_TAG = 9,
    NTRS_BINARY_TLV_REASON_CODE = 10,
    NTRS_BINARY_TLV_CANDIDATE_TYPE = 11,
} ntrs_binary_tlv_type_t;

/**
 * @brief NTRS 二进制帧头。
 */
typedef struct ntrs_binary_frame_header {
    uint32_t magic;
    uint8_t  version;
    uint8_t  frame_type;
    uint8_t  phase;
    uint8_t  flags;
    uint32_t request_id;
    uint32_t sequence;
    uint64_t timestamp_ms;
} ntrs_binary_frame_header_t;

/**
 * @brief NTRS 二进制 TLV 头。
 */
typedef struct ntrs_binary_tlv_header {
    uint16_t type;
    uint16_t length;
} ntrs_binary_tlv_header_t;

/**
 * @brief NTRS 可写帧缓冲。
 */
typedef struct ntrs_binary_frame {
    uint8_t* buffer;
    size_t   capacity;
    size_t   length;
} ntrs_binary_frame_t;

/**
 * @brief NTRS 已解析帧视图。
 */
typedef struct ntrs_binary_frame_view {
    ntrs_binary_frame_header_t header;
    const uint8_t*             payload;
    size_t                     payload_len;
} ntrs_binary_frame_view_t;

/**
 * @brief NTRS 已解析 TLV 视图。
 */
typedef struct ntrs_binary_tlv_view {
    ntrs_binary_tlv_type_t type;
    const uint8_t*         value;
    uint16_t               value_len;
} ntrs_binary_tlv_view_t;

/**
 * @brief NTRS 二进制地址族定义。
 */
typedef enum ntrs_binary_addr_family {
    NTRS_BINARY_ADDR_FAMILY_UNKNOWN = 0,
    NTRS_BINARY_ADDR_FAMILY_IPV4 = 4,
    NTRS_BINARY_ADDR_FAMILY_IPV6 = 6,
} ntrs_binary_addr_family_t;

/**
 * @brief 初始化一个待编码的二进制帧。
 *
 * @param frame 待初始化帧。
 * @param buffer 外部提供的可写缓冲区。
 * @param capacity 缓冲区总长度。
 * @return true 初始化成功。
 * @return false 参数非法或缓冲区过小。
 */
bool ntrs_binary_frame_init(ntrs_binary_frame_t* frame, uint8_t* buffer, size_t capacity);

/**
 * @brief 设置二进制帧头。
 *
 * @param frame 目标帧。
 * @param frame_type 帧类型。
 * @param phase 探测阶段。
 * @param flags 标志位。
 * @param request_id 请求编号。
 * @param sequence 序列号。
 * @param timestamp_ms 时间戳，单位毫秒。
 * @return true 设置成功。
 * @return false 参数非法或内部缓冲区不足。
 */
bool ntrs_binary_frame_set_header(ntrs_binary_frame_t* frame, ntrs_binary_frame_type_t frame_type,
                                  ntrs_binary_phase_t phase, uint8_t flags, uint32_t request_id, uint32_t sequence,
                                  uint64_t timestamp_ms);

/**
 * @brief 向帧中追加一个 TLV 字段。
 *
 * @param frame 目标帧。
 * @param type TLV 字段类型。
 * @param value 字段值起始地址。
 * @param value_len 字段值长度。
 * @return true 追加成功。
 * @return false 参数非法或剩余空间不足。
 */
bool ntrs_binary_frame_add_tlv(ntrs_binary_frame_t* frame, ntrs_binary_tlv_type_t type, const void* value,
                               uint16_t value_len);

/**
 * @brief 解析完整二进制帧。
 *
 * @param data 输入缓冲区。
 * @param data_len 输入缓冲区长度。
 * @param view 输出帧视图。
 * @return true 解析成功。
 * @return false 头部非法、长度非法或魔数/版本不匹配。
 */
bool ntrs_binary_frame_parse(const uint8_t* data, size_t data_len, ntrs_binary_frame_view_t* view);

/**
 * @brief 逐个遍历已解析帧中的 TLV。
 *
 * @param view 已解析帧视图。
 * @param cursor 输入输出游标，初次调用时置 0。
 * @param tlv 输出 TLV 视图。
 * @return true 获取到一个合法 TLV。
 * @return false 没有更多字段或字段格式非法。
 */
bool ntrs_binary_frame_next_tlv(const ntrs_binary_frame_view_t* view, size_t* cursor, ntrs_binary_tlv_view_t* tlv);

/**
 * @brief 在已解析帧中查找首个指定类型的 TLV。
 *
 * @param view 已解析帧视图。
 * @param type 目标 TLV 类型。
 * @param tlv 输出 TLV 视图。
 * @return true 查找成功。
 * @return false 未找到或帧中 TLV 非法。
 */
bool ntrs_binary_frame_find_tlv(const ntrs_binary_frame_view_t* view, ntrs_binary_tlv_type_t type,
                                ntrs_binary_tlv_view_t* tlv);

/**
 * @brief 以二进制地址格式向帧中追加一个地址 TLV。
 *
 * 地址编码格式固定为：
 * - 1 字节地址族：4 表示 IPv4，6 表示 IPv6
 * - 1 字节保留字段，当前固定为 0
 * - 2 字节大端端口
 * - 4 或 16 字节地址正文
 *
 * @param frame 目标帧。
 * @param type TLV 类型，必须为地址类 TLV。
 * @param addr 源地址。
 * @param addr_len 源地址长度。
 * @return true 编码并追加成功。
 * @return false 参数非法、地址族不支持或空间不足。
 */
bool ntrs_binary_frame_add_endpoint_tlv(ntrs_binary_frame_t* frame, ntrs_binary_tlv_type_t type,
                                        const struct sockaddr* addr, socklen_t addr_len);

/**
 * @brief 将地址 TLV 解析为 sockaddr_storage。
 *
 * @param tlv 已解析 TLV。
 * @param addr 输出地址。
 * @param addr_len 输出地址长度。
 * @return true 解析成功。
 * @return false TLV 不是合法地址编码。
 */
bool ntrs_binary_tlv_parse_endpoint(const ntrs_binary_tlv_view_t* tlv, struct sockaddr_storage* addr,
                                    socklen_t* addr_len);

/**
 * @brief 判断帧类型是否合法。
 *
 * @param frame_type 帧类型枚举值。
 * @return true 为已定义帧类型。
 * @return false 为未知或保留值。
 */
bool ntrs_binary_frame_type_is_valid(ntrs_binary_frame_type_t frame_type);

/**
 * @brief 判断探测阶段是否合法。
 *
 * @param phase 探测阶段枚举值。
 * @return true 为已定义阶段。
 * @return false 为未知或保留值。
 */
bool ntrs_binary_phase_is_valid(ntrs_binary_phase_t phase);

#ifdef __cplusplus
}
#endif

#endif
