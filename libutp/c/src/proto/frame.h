#ifndef EULAR_UTP_INTERNAL_FRAME_H
#define EULAR_UTP_INTERNAL_FRAME_H

#include <stddef.h>
#include <stdint.h>

#include "proto/proto.h"
#include "util/error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_FRAME_BIT(type) (UINT32_C(1) << (type))

/* 固定长度帧及可变长度帧固定头部的线上尺寸。 */
#define UTP_FRAME_PATH_SIZE                    9u
#define UTP_FRAME_VERSION_SIZE                 5u
#define UTP_FRAME_HANDSHAKE_DONE_SIZE          9u
#define UTP_FRAME_STREAM_HEADER_SIZE           16u
#define UTP_FRAME_PADDING_HEADER_SIZE          3u
#define UTP_FRAME_CONNECTION_CLOSE_HEADER_SIZE 5u
#define UTP_FRAME_PING_SIZE                    1u
#define UTP_FRAME_RESET_STREAM_SIZE            15u
#define UTP_FRAME_STOP_SENDING_SIZE            7u
#define UTP_FRAME_STREAMS_LIMIT_SIZE           4u
#define UTP_FRAME_CRYPTO_SIZE                  35u
#define UTP_FRAME_SESSION_TOKEN_HEADER_SIZE    10u
#define UTP_FRAME_ACK_FREQUENCY_SIZE           7u
#define UTP_FRAME_TRANSPORT_PARAMS_SIZE        38u
#define UTP_FRAME_HANDSHAKE_DELAY_SIZE         5u
#define UTP_FRAME_MAX_DATA_SIZE                9u
#define UTP_FRAME_MAX_STREAM_DATA_SIZE         13u
#define UTP_FRAME_DATA_BLOCKED_SIZE            9u
#define UTP_FRAME_STREAM_DATA_BLOCKED_SIZE     13u
#define UTP_STREAM_FLAG_NONE                   0x00u
#define UTP_STREAM_FLAG_FIN                    0x01u
// STOP_SENDING 专用错误码：本端不再消费该流，请对端停止发送并回送 RESET_STREAM。
#define UTP_PROTOCOL_STOP_SENDING_CANCELLED                           UINT16_C(1)
#define UTP_TRANSPORT_PARAMS_FLAG_MAX_IDLE_TIMEOUT                    UINT16_C(1) << 0u
#define UTP_TRANSPORT_PARAMS_FLAG_HANDSHAKE_TIMEOUT                   UINT16_C(1) << 1u
#define UTP_TRANSPORT_PARAMS_FLAG_INITIAL_MAX_STREAMS_BIDI            UINT16_C(1) << 2u
#define UTP_TRANSPORT_PARAMS_FLAG_INITIAL_MAX_STREAMS_UNI             UINT16_C(1) << 3u
#define UTP_TRANSPORT_PARAMS_FLAG_ACK_DELAY_EXPONENT                  UINT16_C(1) << 4u
#define UTP_TRANSPORT_PARAMS_FLAG_INITIAL_MAX_DATA                    UINT16_C(1) << 5u
#define UTP_TRANSPORT_PARAMS_FLAG_INITIAL_MAX_STREAM_DATA_BIDI_LOCAL  UINT16_C(1) << 6u
#define UTP_TRANSPORT_PARAMS_FLAG_INITIAL_MAX_STREAM_DATA_BIDI_REMOTE UINT16_C(1) << 7u
#define UTP_TRANSPORT_PARAMS_DEFAULT_FLAGS                                                                    \
    (UTP_TRANSPORT_PARAMS_FLAG_MAX_IDLE_TIMEOUT | UTP_TRANSPORT_PARAMS_FLAG_HANDSHAKE_TIMEOUT |               \
     UTP_TRANSPORT_PARAMS_FLAG_INITIAL_MAX_STREAMS_BIDI | UTP_TRANSPORT_PARAMS_FLAG_INITIAL_MAX_STREAMS_UNI | \
     UTP_TRANSPORT_PARAMS_FLAG_ACK_DELAY_EXPONENT | UTP_TRANSPORT_PARAMS_FLAG_INITIAL_MAX_DATA |              \
     UTP_TRANSPORT_PARAMS_FLAG_INITIAL_MAX_STREAM_DATA_BIDI_LOCAL |                                           \
     UTP_TRANSPORT_PARAMS_FLAG_INITIAL_MAX_STREAM_DATA_BIDI_REMOTE)
#define UTP_TRANSPORT_PARAMS_MAX_ACK_EXPONENT         20u
#define UTP_TRANSPORT_PARAMS_MAX_FLOW_CONTROL         ((UINT64_C(1) << 60) - UINT64_C(1))
#define UTP_ACK_FREQUENCY_MAX_ACK_ELICITING_THRESHOLD 64u
#define UTP_ACK_FREQUENCY_MAX_REORDERING_THRESHOLD    32u
#define UTP_ACK_FREQUENCY_MAX_DELAY_MS                1000u
#define UTP_FRAME_STREAM_TYPE_BIDIRECTIONAL           0u
#define UTP_FRAME_STREAM_TYPE_UNIDIRECTIONAL          1u
#define UTP_FRAME_CRYPTO_TYPE_AES_GCM_128             0u
#define UTP_FRAME_CRYPTO_TYPE_AES_GCM_256             1u

/*
 * 帧的线格式。所有多字节整数均使用网络字节序编码，括号内数字表示字段字节数。
 *
 * STREAM:              type(1), flags(1), data_length(2), stream_id(4), offset(8), data(data_length)
 * ACK:                 type(1), additional_range_count(1), ack_delay(2), first_range_length(4),
 *                      largest_acked(8), ranges[additional_range_count]{gap(4), range_length(4)}
 * PADDING:             type(1), padding_length(2), zero_bytes[padding_length]
 * CONNECTION_CLOSE:    type(1), error_code(2), reason_length(2), reason[reason_length]
 * PING:                type(1)
 * RESET_STREAM:        type(1), error_code(2), stream_id(4), final_size(8)
 * STOP_SENDING:        type(1), error_code(2), stream_id(4)
 * STREAMS_BLOCKED:     type(1), stream_type(1), stream_limit(2)
 * MAX_STREAMS:         type(1), stream_type(1), maximum_streams(2)
 * PATH_CHALLENGE:      type(1), data(8)
 * PATH_RESPONSE:       type(1), data(8)
 * CRYPTO:              type(1), crypto_type(1), reserved(1), ephemeral_public_key(32)
 * SESSION_TOKEN:       type(1), payload_length(1), expires_at_seconds(8), payload[payload_length]
 * ACK_FREQUENCY:       type(1), ack_eliciting_threshold(1), reordering_threshold(1), max_ack_delay_ms(4)
 * VERSION:             type(1), version(4)
 * HANDSHAKE_DONE:      type(1), ack_handshake_packet_number(8)
 * TRANSPORT_PARAMS:    type(1), flags(2), max_idle_timeout(4), handshake_timeout(2),
 *                      initial_max_streams_bidi(2), initial_max_streams_uni(2), ack_delay_exponent(1),
 *                      initial_max_data(8), initial_max_stream_data_bidi_local(8),
 *                      initial_max_stream_data_bidi_remote(8)
 * HANDSHAKE_DELAY:     type(1), delay_time_us(4)
 * MAX_DATA:            type(1), maximum_data(8)
 * MAX_STREAM_DATA:     type(1), stream_id(4), maximum_stream_data(8)
 * DATA_BLOCKED:        type(1), data_limit(8)
 * STREAM_DATA_BLOCKED: type(1), stream_id(4), stream_data_limit(8)
 */
typedef enum utp_frame_type {
    UTP_FRAME_TYPE_INVALID             = 0x00,
    UTP_FRAME_TYPE_STREAM              = 0x01,
    UTP_FRAME_TYPE_ACK                 = 0x02,
    UTP_FRAME_TYPE_PADDING             = 0x03,
    UTP_FRAME_TYPE_CONNECTION_CLOSE    = 0x04,
    UTP_FRAME_TYPE_PING                = 0x05,
    UTP_FRAME_TYPE_RESET_STREAM        = 0x06,
    UTP_FRAME_TYPE_STREAMS_BLOCKED     = 0x07,
    UTP_FRAME_TYPE_MAX_STREAMS         = 0x08,
    UTP_FRAME_TYPE_PATH_CHALLENGE      = 0x09,
    UTP_FRAME_TYPE_PATH_RESPONSE       = 0x0a,
    UTP_FRAME_TYPE_CRYPTO              = 0x0b,
    UTP_FRAME_TYPE_SESSION_TOKEN       = 0x0c,
    UTP_FRAME_TYPE_ACK_FREQUENCY       = 0x0d,
    UTP_FRAME_TYPE_VERSION             = 0x0e,
    UTP_FRAME_TYPE_HANDSHAKE_DONE      = 0x0f,
    UTP_FRAME_TYPE_TRANSPORT_PARAMS    = 0x10,
    UTP_FRAME_TYPE_HANDSHAKE_DELAY     = 0x11,
    UTP_FRAME_TYPE_MAX_DATA            = 0x12,
    UTP_FRAME_TYPE_MAX_STREAM_DATA     = 0x13,
    UTP_FRAME_TYPE_DATA_BLOCKED        = 0x14,
    UTP_FRAME_TYPE_STREAM_DATA_BLOCKED = 0x15,
    UTP_FRAME_TYPE_STOP_SENDING        = 0x16,
    UTP_FRAME_TYPE_MAX                 = 0x17
} utp_frame_type_t;

typedef struct utp_packet_view {
    utp_packet_header_t header;          // 已解码的固定包头
    const uint8_t*      payload;         // 指向原始包 payload 的零拷贝视图
    size_t              payload_length;  // payload 总长度
    uint32_t            frame_types;     // 扫描得到的帧类型位图
} utp_packet_view_t;

/* PATH_CHALLENGE/PATH_RESPONSE：type(1), data(8)。 */
typedef struct utp_frame_path {
    uint8_t data[8];  // 路径验证随机质询或响应值
} utp_frame_path_t;

/* CRYPTO：type(1), crypto_type(1), reserved(1), ephemeral_public_key(32)。 */
typedef struct utp_frame_crypto {
    uint8_t crypto_type;               // 协商的密钥交换和 AEAD 类型
    uint8_t ephemeral_public_key[32];  // 对端 X25519 临时公钥
} utp_frame_crypto_t;

/* VERSION：type(1), version(4)。 */
typedef struct utp_frame_version {
    uint32_t version;  // 协议版本号
} utp_frame_version_t;

/* HANDSHAKE_DONE：type(1), ack_handshake_packet_number(8)。 */
typedef struct utp_frame_handshake_done {
    uint64_t ack_handshake_packet_number;  // 被确认的对端握手包号
} utp_frame_handshake_done_t;

/* HANDSHAKE_DELAY：type(1), delay_time_us(4)。 */
typedef struct utp_frame_handshake_delay {
    uint32_t delay_time_us;  // 本端处理对应握手包的耗时，单位 us
} utp_frame_handshake_delay_t;

/* STREAM：type(1), flags(1), data_length(2), stream_id(4), offset(8), data[data_length]。 */
typedef struct utp_frame_stream {
    uint8_t        flags;        // FIN 等 STREAM 标志位
    uint32_t       stream_id;    // 目标流 ID
    uint64_t       offset;       // 数据在流内的起始偏移
    const uint8_t* data;         // 指向原始包内的数据视图
    uint16_t       data_length;  // 数据长度
} utp_frame_stream_t;

/* SESSION_TOKEN：type(1), payload_length(1), expires_at_seconds(8), payload[payload_length]。 */
typedef struct utp_frame_session_token {
    const uint8_t* payload;             // 票据 payload 的零拷贝视图
    uint64_t       expires_at_seconds;  // 绝对失效时间，Unix 秒
    uint8_t        payload_length;      // 票据 payload 长度
} utp_frame_session_token_t;

/* CONNECTION_CLOSE：type(1), error_code(2), reason_length(2), reason[reason_length]。 */
typedef struct utp_frame_connection_close {
    uint16_t       error_code;     // 协议关闭错误码
    const uint8_t* reason;         // 关闭原因的零拷贝视图
    uint16_t       reason_length;  // 关闭原因长度
} utp_frame_connection_close_t;

/* RESET_STREAM：type(1), error_code(2), stream_id(4), final_size(8)。 */
typedef struct utp_frame_reset_stream {
    uint16_t error_code;  // 流重置错误码
    uint32_t stream_id;   // 被重置流 ID
    uint64_t final_size;  // 发送方最终流偏移
} utp_frame_reset_stream_t;

/* STOP_SENDING：type(1), error_code(2), stream_id(4)。 */
typedef struct utp_frame_stop_sending {
    uint16_t error_code;  // 请求发送方使用的流错误码
    uint32_t stream_id;   // 要求停止发送的流 ID
} utp_frame_stop_sending_t;

/* STREAMS_BLOCKED/MAX_STREAMS：type(1), stream_type(1), stream_limit(2)。 */
typedef struct utp_frame_streams_limit {
    uint16_t stream_limit;  // 对应方向允许创建的累计流数
    uint8_t  stream_type;   // 双向或单向流类型
} utp_frame_streams_limit_t;

/* MAX_DATA：type(1), maximum_data(8)。 */
typedef struct utp_frame_max_data {
    uint64_t maximum_data;  // 连接级最大接收偏移
} utp_frame_max_data_t;

/* MAX_STREAM_DATA：type(1), stream_id(4), maximum_stream_data(8)。 */
typedef struct utp_frame_max_stream_data {
    uint32_t stream_id;            // 流 ID
    uint64_t maximum_stream_data;  // 该流最大接收偏移
} utp_frame_max_stream_data_t;

/* DATA_BLOCKED：type(1), data_limit(8)。 */
typedef struct utp_frame_data_blocked {
    uint64_t data_limit;  // 发送方受阻时观察到的连接级额度
} utp_frame_data_blocked_t;

/* STREAM_DATA_BLOCKED：type(1), stream_id(4), stream_data_limit(8)。 */
typedef struct utp_frame_stream_data_blocked {
    uint32_t stream_id;          // 受阻流 ID
    uint64_t stream_data_limit;  // 发送方受阻时观察到的流级额度
} utp_frame_stream_data_blocked_t;

/* ACK_FREQUENCY：type(1), ack_eliciting_threshold(1), reordering_threshold(1), max_ack_delay_ms(4)。 */
typedef struct utp_frame_ack_frequency {
    uint32_t max_ack_delay_ms;         // 最大 ACK 延迟，单位 ms
    uint8_t  ack_eliciting_threshold;  // 累计多少 ACK-eliciting 包后立即 ACK
    uint8_t  reordering_threshold;     // 检测到乱序缺口后的立即 ACK 阈值
} utp_frame_ack_frequency_t;

/* TRANSPORT_PARAMS：type(1), flags(2), idle_timeout(4), handshake_timeout(2), streams(2+2), exponent(1), credits(8*3)。
 */
typedef struct utp_frame_transport_params {
    uint64_t initial_max_data;                     // 对端可发送的连接级初始额度
    uint64_t initial_max_stream_data_bidi_local;   // 对端发起双向流的初始额度
    uint64_t initial_max_stream_data_bidi_remote;  // 本端发起双向流的初始额度
    uint32_t max_idle_timeout_ms;                  // 最大空闲超时，单位 ms
    uint16_t flags;                                // 传输参数标志位
    uint16_t handshake_timeout_ms;                 // 握手超时，单位 ms
    uint16_t initial_max_streams_bidi;             // 初始允许创建的双向流数量
    uint16_t initial_max_streams_uni;              // 初始允许创建的单向流数量
    uint8_t  ack_delay_exponent;                   // ACK 延迟编码指数
} utp_frame_transport_params_t;

/** @brief 测量首个帧的完整线上长度与类型，不修改输入缓冲区。 */
utp_internal_error_t utp_frame_measure(const uint8_t* frame, size_t available, uint8_t* frame_type,
                                       size_t* frame_length);
/** @brief 扫描 payload 中的所有帧并返回帧类型位图。 */
utp_internal_error_t utp_frame_scan(const uint8_t* payload, size_t payload_length, uint32_t* frame_types);
/** @brief 解析固定包头和 payload 边界，构造零拷贝包视图。 */
utp_internal_error_t utp_packet_view_decode(utp_packet_view_t* view, const uint8_t* packet, size_t packet_length);
/** @brief 从包视图当前位置读取下一帧，并推进 @p offset。 */
utp_internal_error_t utp_packet_view_next_frame(const utp_packet_view_t* view, size_t* offset, uint8_t* frame_type,
                                                const uint8_t** frame_data, size_t* frame_length);
/** @brief 编码 PATH_CHALLENGE 或 PATH_RESPONSE 帧。 */
utp_internal_error_t utp_frame_path_encode(uint8_t* buffer, size_t capacity, uint8_t type,
                                           const utp_frame_path_t* path);
/** @brief 解码并校验指定类型的路径验证帧。 */
utp_internal_error_t utp_frame_path_decode(utp_frame_path_t* path, const uint8_t* buffer, size_t length,
                                           uint8_t expected_type);
/** @brief 编码明文握手阶段携带的 CRYPTO 帧。 */
utp_internal_error_t utp_frame_crypto_encode(uint8_t* buffer, size_t capacity, const utp_frame_crypto_t* crypto);
/** @brief 解码并校验 CRYPTO 帧的保留字段与算法类型。 */
utp_internal_error_t utp_frame_crypto_decode(utp_frame_crypto_t* crypto, const uint8_t* buffer, size_t length);
/** @brief 编码协议版本协商帧。 */
utp_internal_error_t utp_frame_version_encode(uint8_t* buffer, size_t capacity, const utp_frame_version_t* version);
/** @brief 解码协议版本协商帧。 */
utp_internal_error_t utp_frame_version_decode(utp_frame_version_t* version, const uint8_t* buffer, size_t length);
/** @brief 编码确认服务端 Handshake 包号的 HANDSHAKE_DONE 帧。 */
utp_internal_error_t utp_frame_handshake_done_encode(uint8_t* buffer, size_t capacity,
                                                     const utp_frame_handshake_done_t* done);
/** @brief 解码 HANDSHAKE_DONE 帧。 */
utp_internal_error_t utp_frame_handshake_done_decode(utp_frame_handshake_done_t* done, const uint8_t* buffer,
                                                     size_t length);
/** @brief 编码握手阶段本端处理耗时。 */
utp_internal_error_t utp_frame_handshake_delay_encode(uint8_t* buffer, size_t capacity,
                                                      const utp_frame_handshake_delay_t* delay);
/** @brief 解码握手阶段本端处理耗时。 */
utp_internal_error_t utp_frame_handshake_delay_decode(utp_frame_handshake_delay_t* delay, const uint8_t* buffer,
                                                      size_t length);
/** @brief 编码完整 STREAM 帧，包括数据副本。 */
utp_internal_error_t utp_frame_stream_encode(uint8_t* buffer, size_t capacity, const utp_frame_stream_t* stream);
/** @brief 仅编码 STREAM 固定头，用于零拷贝发送数据切片。 */
utp_internal_error_t utp_frame_stream_header_encode(uint8_t* buffer, size_t capacity, uint8_t flags, uint32_t stream_id,
                                                    uint64_t offset, uint16_t data_length);
/** @brief 解码 STREAM 帧；返回的数据指针借用输入缓冲区。 */
utp_internal_error_t utp_frame_stream_decode(utp_frame_stream_t* stream, const uint8_t* buffer, size_t length);
/** @brief 编码 SESSION_TOKEN 帧。 */
utp_internal_error_t utp_frame_session_token_encode(uint8_t* buffer, size_t capacity,
                                                    const utp_frame_session_token_t* token);
/** @brief 解码 SESSION_TOKEN 帧；票据指针借用输入缓冲区。 */
utp_internal_error_t utp_frame_session_token_decode(utp_frame_session_token_t* token, const uint8_t* buffer,
                                                    size_t length);
/** @brief 编码指定长度的零填充 PADDING 帧。 */
utp_internal_error_t utp_frame_padding_encode(uint8_t* buffer, size_t capacity, uint16_t padding_length);
/** @brief 解码 PADDING 帧并验证所有填充字节为零。 */
utp_internal_error_t utp_frame_padding_decode(uint16_t* padding_length, const uint8_t* buffer, size_t length);
/** @brief 编码 CONNECTION_CLOSE 帧及可选关闭原因。 */
utp_internal_error_t utp_frame_connection_close_encode(uint8_t* buffer, size_t capacity,
                                                       const utp_frame_connection_close_t* close);
/** @brief 解码 CONNECTION_CLOSE 帧；原因指针借用输入缓冲区。 */
utp_internal_error_t utp_frame_connection_close_decode(utp_frame_connection_close_t* close, const uint8_t* buffer,
                                                       size_t length);
/** @brief 编码 RESET_STREAM 帧。 */
utp_internal_error_t utp_frame_reset_stream_encode(uint8_t* buffer, size_t capacity,
                                                   const utp_frame_reset_stream_t* reset);
/** @brief 解码 RESET_STREAM 帧。 */
utp_internal_error_t utp_frame_reset_stream_decode(utp_frame_reset_stream_t* reset, const uint8_t* buffer,
                                                   size_t length);
/** @brief 编码 STOP_SENDING 帧。 */
utp_internal_error_t utp_frame_stop_sending_encode(uint8_t* buffer, size_t capacity,
                                                   const utp_frame_stop_sending_t* stop);
/** @brief 解码 STOP_SENDING 帧。 */
utp_internal_error_t utp_frame_stop_sending_decode(utp_frame_stop_sending_t* stop, const uint8_t* buffer,
                                                   size_t length);
/** @brief 编码 STREAMS_BLOCKED 帧。 */
utp_internal_error_t utp_frame_streams_blocked_encode(uint8_t* buffer, size_t capacity,
                                                      const utp_frame_streams_limit_t* blocked);
/** @brief 解码 STREAMS_BLOCKED 帧。 */
utp_internal_error_t utp_frame_streams_blocked_decode(utp_frame_streams_limit_t* blocked, const uint8_t* buffer,
                                                      size_t length);
/** @brief 编码 MAX_STREAMS 帧。 */
utp_internal_error_t utp_frame_max_streams_encode(uint8_t* buffer, size_t capacity,
                                                  const utp_frame_streams_limit_t* maximum);
/** @brief 解码 MAX_STREAMS 帧。 */
utp_internal_error_t utp_frame_max_streams_decode(utp_frame_streams_limit_t* maximum, const uint8_t* buffer,
                                                  size_t length);
/** @brief 编码连接级发送额度 MAX_DATA 帧。 */
utp_internal_error_t utp_frame_max_data_encode(uint8_t* buffer, size_t capacity, const utp_frame_max_data_t* max_data);
/** @brief 解码连接级发送额度 MAX_DATA 帧。 */
utp_internal_error_t utp_frame_max_data_decode(utp_frame_max_data_t* max_data, const uint8_t* buffer, size_t length);
/** @brief 编码流级发送额度 MAX_STREAM_DATA 帧。 */
utp_internal_error_t utp_frame_max_stream_data_encode(uint8_t* buffer, size_t capacity,
                                                      const utp_frame_max_stream_data_t* max_stream_data);
/** @brief 解码流级发送额度 MAX_STREAM_DATA 帧。 */
utp_internal_error_t utp_frame_max_stream_data_decode(utp_frame_max_stream_data_t* max_stream_data,
                                                      const uint8_t* buffer, size_t length);
/** @brief 编码连接级受阻通知 DATA_BLOCKED 帧。 */
utp_internal_error_t utp_frame_data_blocked_encode(uint8_t* buffer, size_t capacity,
                                                   const utp_frame_data_blocked_t* blocked);
/** @brief 解码连接级受阻通知 DATA_BLOCKED 帧。 */
utp_internal_error_t utp_frame_data_blocked_decode(utp_frame_data_blocked_t* blocked, const uint8_t* buffer,
                                                   size_t length);
/** @brief 编码流级受阻通知 STREAM_DATA_BLOCKED 帧。 */
utp_internal_error_t utp_frame_stream_data_blocked_encode(uint8_t* buffer, size_t capacity,
                                                          const utp_frame_stream_data_blocked_t* blocked);
/** @brief 解码流级受阻通知 STREAM_DATA_BLOCKED 帧。 */
utp_internal_error_t utp_frame_stream_data_blocked_decode(utp_frame_stream_data_blocked_t* blocked,
                                                          const uint8_t* buffer, size_t length);
/** @brief 编码 ACK_FREQUENCY，并规范化零值与协议上限。 */
utp_internal_error_t utp_frame_ack_frequency_encode(uint8_t* buffer, size_t capacity,
                                                    const utp_frame_ack_frequency_t* frequency);
/** @brief 解码 ACK_FREQUENCY，并规范化零值与协议上限。 */
utp_internal_error_t utp_frame_ack_frequency_decode(utp_frame_ack_frequency_t* frequency, const uint8_t* buffer,
                                                    size_t length);
/** @brief 编码固定长度 TRANSPORT_PARAMS。 */
utp_internal_error_t utp_frame_transport_params_encode(uint8_t* buffer, size_t capacity,
                                                       const utp_frame_transport_params_t* params);
/** @brief 解码并校验 TRANSPORT_PARAMS 的 flags、ACK 指数和流控上限。 */
utp_internal_error_t utp_frame_transport_params_decode(utp_frame_transport_params_t* params, const uint8_t* buffer,
                                                       size_t length);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_INTERNAL_FRAME_H
