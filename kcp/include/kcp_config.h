#ifndef __KCP_CONFIG_H__
#define __KCP_CONFIG_H__

#include <stdint.h>

// kcp 交流号标志, 即最多支持65535个会话
#define KCP_CONV_FLAG       0xFFFF0000
#define KCP_BITMAP_SIZE     65535

#define KCP_HEADER_SIZE     32
#define KCP_PACKET_COUNT    32

enum ConfigKey {
    CONFIG_KEY_NODELAY  = 0b0001,
    CONFIG_KEY_INTERVAL = 0b0010,
    CONFIG_KEY_RESEND   = 0b0100,
    CONFIG_KEY_NC       = 0b1000,
    CONFIG_KEY_ALL      = 0b1111,
};
typedef uint32_t em_config_key_t;

enum IOControl {
    IOCTL_RECEIVE_TIMEOUT,
    IOCTL_MTU_PROBE_TIMEOUT,
    IOCTL_KEEPALIVE_TIMEOUT,
    IOCTL_KEEPALIVE_INTERVAL,
    IOCTL_SYN_RETRIES,
    IOCTL_FIN_RETRIES,
    IOCTL_WINDOW_SIZE,
};
typedef uint32_t em_ioctl_t;

typedef struct KcpConfig {
    int32_t nodelay;    // 是否启用 nodelay模式, 0不启用; 1启用
    int32_t interval;   // 协议内部工作的 interval, 单位毫秒, 比如 10ms 或者 20ms
    int32_t resend;     // 快速重传模式, 默认0关闭, 可以设置2 (2次ACK跨越将会直接重传)
    int32_t nc;         // 是否关闭流控, 默认是0代表不关闭, 1代表关闭
} kcp_config_t;

#define KCP_CONFIG_NORMAL   (kcp_config_t){0, 40, 0, 0}
#define KCP_CONFIG_FAST     (kcp_config_t){0, 30, 2, 1}
#define KCP_CONFIG_FAST_2   (kcp_config_t){1, 20, 2, 1}
#define KCP_CONFIG_FAST_3   (kcp_config_t){1, 10, 2, 1}

static const uint32_t IKCP_RTO_NDL = 30;    // no delay min rto
static const uint32_t IKCP_RTO_MIN = 100;   // normal min rto
static const uint32_t IKCP_RTO_DEF = 200;

static const uint32_t IKCP_WND_SND = 32;
static const uint32_t IKCP_WND_RCV = 128;           // must >= max fragment size
static const uint32_t IKCP_MTU_DEF = 1400;
static const uint32_t IKCP_ACK_FAST = 3;
static const uint32_t IKCP_INTERVAL = 100;
static const uint32_t IKCP_OVERHEAD = 24;
static const uint32_t IKCP_DEADLINK = 20;
static const uint32_t IKCP_THRESH_INIT = 2;
static const uint32_t IKCP_THRESH_MIN = 2;

/////////////////
static const uint32_t   KCP_RTO_MAX     = 6000;     // ms
static const uint32_t   KCP_ASK_SEND    = 0b0001;   // need to send IKCP_CMD_WASK
static const uint32_t   KCP_ASK_TELL    = 0b0010;   // need to send KCP_CMD_WINS
static const uint32_t   KCP_PING_RECV   = 0b0100;   // 

static const uint32_t   KCP_WND_RCV     = 128;      // must >= max fragment size
#define                 KCP_MAX_PACKET_SIZE     ((576 - 20 - 8 - KCP_HEADER_SIZE) * KCP_WND_RCV) // 一次发送的最大字节数, frg [0, KCP_WND_RCV - 1]

static const uint32_t   KCP_INTERVAL_MAX        = 500;  // 协议内部发送数据的最大间隔
static const uint32_t   KCP_INTERVAL_MIN        = 10;   // 协议内部发送数据的最小间隔
static const int32_t    KCP_FASTACK_LIMIT       = 5;    // 快速重传ACK最大跳过次数
static const uint32_t   KCP_RETRANSMISSION_MAX  = 5;    // 最大重传次数
static const uint32_t   KCP_KEEPALIVE_TIMEOUT   = 10;   // 心跳超时时间 10 * rtt
static const uint32_t   KCP_KEEPALIVE_INTERVAL  = 10000;// 心跳间隔时间
static const uint32_t   KCP_KEEPALIVE_TIMES     = 5;    // 心跳超时最大次数
static const uint32_t   KCP_PROBE_INIT          = 7000; // 探测窗口大小的初始时间
static const uint32_t   KCP_PROBE_LIMIT         = 120000; // 探测窗口大小的最大时间

#define DEFAULT_RECEIVE_TIMEOUT     1000
#define DEFAULT_KEEPALIVE_TIMEOUT   5000
#define DEFAULT_KEEPALIVE_INTERVAL  10000
#define DEFAULT_KEEPALIVE_RETRIES   3
#define DEFAULT_SYN_FIN_RETRIES     2
#define DEFAULT_MTU_PROBE_TIMEOUT   1500

#endif // __KCP_CONFIG_H__
