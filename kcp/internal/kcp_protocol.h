#ifndef __KCP_PROTOCOL_H__
#define __KCP_PROTOCOL_H__

#include <stdint.h>
#include <stddef.h>

#include <event2/event.h>

#include "list.h"

#include "kcp_def.h"
#include "connection_set.h"
#include "kcp_config.h"
#include "kcpp.h"
#include "bitmap.h"

/// @brief KCP协议命令类型
enum KcpCommand {
    KCP_CMD_SYN = 1,        // SYN
    KCP_CMD_ACK,            // ACK
    KCP_CMD_PUSH,           // PUSH
    KCP_CMD_WASK,           // Window Probe (ask)
    KCP_CMD_WINS,           // Window Size (tell)
    KCP_CMD_PING,           // PING
    KCP_CMD_PONG,           // PONG
    KCP_CMD_MTU_PROBE,      // MTU probe
    KCP_CMD_MTU_ACK,        // MTU probe
    KCP_CMD_FIN,            // FIN
    KCP_CMD_RST,            // RST
};

// 扩展命令
enum KcpExtendedCommand {
    KCP_CMD_OPT = 0b0001,
};

enum KcpConnectionState {
    KCP_STATE_DISCONNECTED = 1,
    KCP_STATE_SYN_SENT,
    KCP_STATE_SYN_RECEIVED,
    KCP_STATE_CONNECTED,
    KCP_STATE_FIN_SENT,
    KCP_STATE_FIN_RECEIVED,
};
typedef int32_t kcp_connection_state_t;

enum KcpOptionTag {
    KCP_OPTION_TAG_MTU = 1,
};
typedef int32_t kcp_option_tag_t;
#define KCP_OPTION_TAG_MTU_LEN 6

typedef struct KcpOption {
    struct list_head node; // 链表节点
    int8_t  tag;
    int8_t  length;

    union {
        uint64_t    u64_value; // 64位整数值
        char*       buf_value; // 缓冲区值
    };
} kcp_option_t;

typedef struct KcpProtoHeader {
    struct list_head node_list;  // 链表节点

    uint32_t    conv;       // 会话ID
#if defined(BYTE_ORDER) && BYTE_ORDER == LITTLE_ENDIAN
    uint8_t     cmd:4;      // 命令类型 (low 4 bit)
    uint8_t     opt:4;      // 分片序号 (high 4 bit)
#else
    uint8_t     opt:4;      // 分片序号
    uint8_t     cmd:4;      // 命令类型
#endif
    uint8_t     frg;        // 分片序号
    uint16_t    wnd;        // 窗口大小

    union {
        struct {
            uint64_t    ts;     // packet发送时间戳
            uint32_t    sn;     // 序号
            uint32_t    psn;    // 包序列
            uint32_t    una;    // 未确认序号
            uint32_t    len;    // 数据长度
            char*       data;   // 数据
        } packet_data;

        struct { // NOTE 用于在三次握手阶段建立连接计算rtt
            uint64_t    packet_ts;  // 接收此packet的时间戳
            uint64_t    ts;         // 发送当前packet的时间戳
            uint32_t    packet_sn;  // 包序列
            uint32_t    rand_sn;    // 随机序列
        } syn_fin_data;

        struct {
            uint64_t    packet_ts;  // 接收此packet的时间戳
            uint64_t    ack_ts;     // 发送ack的时间戳
            uint32_t    sn;         // 序号
            uint32_t    una;        // 未确认序号
        } ack_data;

        struct {
            uint64_t    packet_ts;  // 接收此packet的时间戳
            uint64_t    ts;         // 发送 PING/PONG 的时间戳
            uint64_t    sn;         // 随机序列
        } ping_data;
    };

    struct list_head options;
} kcp_proto_header_t;

/// @brief KCP报文段
typedef struct KcpSengment {
    struct list_head    node_list;
    struct rb_node      node_rbtree;

    uint32_t conv;      // 会话ID
    uint32_t cmd;       // 命令
    uint32_t frg;       // 分片序号
    uint32_t wnd;       // 窗口大小
    uint64_t ts;        // 时间戳
    uint32_t sn;        // 序号
    uint32_t psn;       // 包序列
    uint32_t una;       // 未确认序号
    uint32_t len;       // 数据长度
    uint32_t rto;       // 超时重传时间
    uint64_t resendts;  // 重传时间戳
    uint32_t fastack;   // 快速重传
    uint32_t xmit;      // 传输次数
    char     data[1];   // 数据
} kcp_segment_t;

typedef struct KcpAck {
    struct list_head node;
    uint32_t sn;    // 序号
    uint32_t psn;   // 包序列
    uint64_t ts;    // 接收包的时间戳
} kcp_ack_t;

typedef void (*kcp_read_cb_t)(struct KcpConnection *, const kcp_proto_header_t *, const sockaddr_t *);
// NOTE timestamp == 0 表示写事件触发, 否则表示超时事件触发
typedef int32_t (*kcp_write_cb_t)(struct KcpConnection *, uint64_t);

/// @brief KCP控制块
typedef struct KcpConnection {
    struct rb_node      node_rbtree; // for set
    struct list_node    node_list;   // for list

    // 基础配置
    uint32_t conv;          // 会话ID，用于标识一个会话
    uint32_t mtu;           // 最大传输单元
    uint32_t mss;           // 报文段大小
    uint32_t mss_min;       // 最小报文段大小

    // 发送和接收序号
    uint32_t snd_una;       // 第一个未确认的包序号
    uint32_t snd_nxt;       // 下一个待发送的包序号
    uint32_t rcv_nxt;       // 待接收的下一个包序号
    uint32_t psn_nxt;       // 下一个待发送的包序号

    // 时间戳相关
    uint32_t ts_recent;     // 最近一次收到包的时间戳
    uint32_t ts_lastack;    // 最近一次收到ACK的时间戳
    uint64_t ts_flush;      // 下次刷新时间戳(ms)
    uint32_t ssthresh;      // 慢启动阈值，默认为IKCP_THRESH_INIT(2)

    // RTT相关
    int32_t rx_rttval;      // RTT 的偏差, 用于计算 RTT 的波动
    int32_t rx_srtt;        // 平滑的 RTT 值, 用于计算平均 RTT(us)
    int32_t rx_rto;         // 超时重传时间，初始为 KCP_RTO_DEF(200ms)
    int32_t rx_minrto;      // 最小重传超时时间，默认为 KCP_RTO_MIN(100ms)

    // 窗口相关
    uint32_t snd_wnd;       // 发送窗口大小，默认128
    uint32_t rcv_wnd;       // 接收窗口大小，默认256
    uint32_t rmt_wnd;       // 远端窗口大小，默认256
    uint32_t cwnd;          // 拥塞窗口大小，初始为0
    uint32_t probe;         // 探测标志，用于窗口探测

    // 配置标志
    uint32_t nodelay;       // 是否启用nodelay模式，0=不启用
    uint32_t updated;       // 是否调用过update

    // 探测相关
    uint32_t win_ts_probe;  // 下次widnow size探测时间
    uint32_t probe_wait;    // 探测等待时间

    // 时间相关
    uint32_t current;       // 当前时间
    uint32_t interval;      // 内部更新时间间隔，默认100ms

    // 队列计数器
    int32_t nrcv_buf;           // 接收缓存中的包数量
    int32_t nsnd_buf;           // 发送缓存中的包数量
    int32_t nsnd_buf_unused;    // 未使用的发送缓存数量
    int32_t nrcv_buf_unused;    // 未使用的接收队列数量
    int32_t nrcv_que;           // 接收队列中的包数量
    int32_t nsnd_que;           // 发送队列中的包数量

    // packet 计数
    uint32_t nsnd_pkt_next;     // 下一个待发送发送包序号
    uint32_t incr;              // 可发送的最大数据量

    // 数据队列
    struct list_head    snd_queue;      // 发送队列
    struct list_head    snd_buf;        // 发送缓存
    struct list_head    snd_buf_unused; // 未使用的发送缓存

    struct list_head    rcv_queue;      // 接收队列
    struct list_head    rcv_buf;        // 接收缓存
    struct list_head    rcv_buf_unused; // 未使用的接收缓存

    // ACK相关
    struct list_head    ack_item;   // ACK列表项
    struct list_head    ack_unused; // 未使用的ACK列表项

    // 临时缓存
    char *buffer; // 存放ACK或PING等数据

    // 快速重传相关
    int32_t fastresend; // 触发快速重传的重复ACK个数
    int32_t fastlimit;  // 快速重传次数限制，默认 KCP_FASTACK_LIMIT(5)

    // 其他配置
    int32_t nocwnd;     // 是否关闭拥塞控制，0=不关闭

    // base
    struct KcpContext*      kcp_ctx;
    struct event*           syn_timer_event;
    struct event*           fin_timer_event;
    bool                    need_write_timer_event;
    kcp_connection_state_t  state;
    uint32_t                receive_timeout;
    uint32_t                syn_fin_sn; // unused
    int32_t                 syn_retries;
    uint32_t                fin_retries;
    sockaddr_t              remote_host;

    // syn
    struct KcpSYNNode*      syn_node;
    struct list_node        kcp_proto_header_list; // for syn fin

    // mtu
    struct KcpMtuProbeCtx*  mtu_probe_ctx;

    // ping
    struct KcpPingCtx*      ping_ctx;

    // socket callback
    kcp_read_cb_t           read_cb;
    kcp_write_cb_t          write_cb;

    // user callback
    on_kcp_read_event_t     read_event_cb;

    // statistics
    uint32_t                ping_count; // ping次数
    uint32_t                pong_count; // pong次数
    uint64_t                tx_bytes;   // 发送字节数
    uint64_t                rtx_bytes;  // 重传字节数
} kcp_connection_t;

typedef struct KcpFunctionCallback {
    on_kcp_connected_t      on_connected;
    on_kcp_connect_t        on_connect; // 收到syn包回调
    on_kcp_accepted_t       on_accepted;
    on_kcp_closed_t         on_closed;
    on_kcp_error_t          on_error;
} kcp_function_callback_t;

typedef struct KcpSYNNode {
    struct list_head    node;
    uint32_t            conv;
    uint32_t            rand_sn;    // 本机发送的sn
    uint32_t            packet_sn;  // 对端sn
    uint64_t            packet_ts;  // 对端时间戳
    uint64_t            ts;         // 发送syn的时间戳
    sockaddr_t          remote_host;
    struct list_head    options;    // 选项列表
} kcp_syn_node_t;

typedef struct KcpContext {
    socket_t                    sock;
    sockaddr_t                  local_addr;
    kcp_function_callback_t     callback;

    uint32_t                    nic_mtu;
    bitmap_t                    conv_bitmap;
    struct list_head            syn_queue;
    connection_set_t            connection_set;
    struct event_base*          event_loop;
    struct event*               read_event;
    struct event*               write_event;
    struct event*               write_timer_event;
    struct list_head            conn_write_event_queue;
    void*                       user_data;
    char*                       read_buffer;
    size_t                      read_buffer_size;
} kcp_context_t;

////////////////////////////////////////MTU探测////////////////////////////////////////
// MTU探测回调
typedef void (*on_probe_completed_t)(kcp_connection_t *kcp_conn, uint32_t mtu, int32_t code);

// MTU响应
typedef void (*on_probe_response_t)(kcp_connection_t *kcp_conn);

typedef struct KcpMtuProbeCtx {
    struct event*           probe_timeout_event;    // MTU探测超时事件
    on_probe_completed_t    on_probe_completed;     // MTU探测完成回调
    on_probe_response_t     on_probe_response;      // MTU响应回调, 用以下一步探测
    uint32_t                mtu_best;               // 最佳MTU
    uint32_t                mtu_current;            // 当前MTU
    uint32_t                mtu_lbound;             // MTU下限
    uint32_t                mtu_ubound;             // MTU上限
    uint32_t                timeout;                // 超时时间
    uint16_t                retries;                // 重试次数
    uint32_t                prev_sn;                // 上一个序号
    uint64_t                hash;                   // hash值
    char*                   probe_buf;              // 探测数据
} mtu_probe_ctx_t;

typedef struct PingSession {
    struct list_head node;
    uint64_t packet_sn; // ping packet's sn
    uint64_t packet_ts; // ping packet's timestamp
} ping_session_t;

typedef struct KcpPingCtx {
    uint32_t                keepalive_timeout;      // keepalive超时时间 (us)
    uint32_t                keepalive_interval;     // keepalive间隔时间 (us)
    uint64_t                keepalive_next_ts;      // 下次需要发送ping包的时间戳 (us)
    uint16_t                keepalive_retries;      // keepalive 配置的重试次数
    uint16_t                keepalive_xretries;     // keepalive 重试次数
    uint32_t                keepalive_rtt;          // keepalive RTT
    uint64_t                keepalive_sn;           // keepalive序号
    uint64_t                keepalive_packet_ts;    // keepalive packet时间戳
    struct list_head        ping_request_queue;     // ping队列
} ping_ctx_t;


EXTERN_C_BEGIN

void kcp_connection_init(kcp_connection_t *kcp_conn, const sockaddr_t *remote_host, struct KcpContext* kcp_ctx);

void kcp_connection_destroy(kcp_connection_t *kcp_conn);

int32_t kcp_proto_parse(kcp_proto_header_t *kcp_header, const char **data, size_t data_size);

int32_t kcp_proto_header_encode(const kcp_proto_header_t *kcp_header, char *buffer, size_t buffer_size);

int32_t kcp_segment_encode(const kcp_segment_t *segment, char *buffer, size_t buffer_size);

int32_t kcp_input_pcaket(kcp_connection_t *kcp_conn, const kcp_proto_header_t *kcp_header);

void on_kcp_syn_received(struct KcpContext *kcp_ctx, const sockaddr_t *addr);

kcp_segment_t *kcp_segment_send_get(kcp_connection_t *kcp_conn);
void kcp_segment_send_put(kcp_connection_t *kcp_conn, kcp_segment_t *segment);

kcp_segment_t *kcp_segment_recv_get(kcp_connection_t *kcp_conn);
void kcp_segment_recv_put(kcp_connection_t *kcp_conn, kcp_segment_t *segment);

EXTERN_C_END

#endif // __KCP_PROTOCOL_H__
