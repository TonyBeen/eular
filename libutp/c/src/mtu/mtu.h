#ifndef EULAR_UTP_INTERNAL_MTU_H
#define EULAR_UTP_INTERNAL_MTU_H

#include <stdbool.h>
#include <stdint.h>

#include "socket/address.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_MTU_MAX                UINT16_MAX
#define UTP_MTU_ETHERNET_MAX       1500u
#define UTP_MTU_DEFAULT_MIN        1280u
#define UTP_MTU_DEFAULT_BASE       1400u
#define UTP_MTU_DEFAULT_PROBE_STEP 16u

typedef enum utp_mtu_probe_phase {
    UTP_MTU_PROBE_PHASE_LADDER = 0,
    UTP_MTU_PROBE_PHASE_BINARY,
    UTP_MTU_PROBE_PHASE_BLACKHOLE_BASE,
    UTP_MTU_PROBE_PHASE_STABLE
} utp_mtu_probe_phase_t;

typedef struct utp_mtu_config {
    bool     enabled;                   // 是否启用 DPLPMTUD
    uint16_t mtu_min;                   // 可服务 MTU 下限
    uint16_t mtu_max;                   // 探测 MTU 上限
    uint16_t mtu_base;                  // 初始安全 MTU
    uint32_t probe_interval_seconds;    // 稳定期探测间隔
    uint16_t probe_step;                // 阶梯探测步长
    uint16_t probe_timeout_ms;          // 单次探测超时
    uint8_t  probe_retries;             // 单个探测额外重试次数
    uint8_t  blackhole_loss_threshold;  // 判定黑洞的连续大包丢失次数
    uint16_t blackhole_loss_window_ms;  // 黑洞丢失观察窗口
    uint16_t blackhole_cooldown_ms;     // 黑洞恢复冷却时间
} utp_mtu_config_t;

#define UTP_MTU_CONFIG_INIT {true, 1280u, 1500u, 1400u, 300u, 16u, 2000u, 1u, 3u, 3000u, 5000u}

typedef struct utp_mtu_discovery {
    uint64_t              next_probe_time_ms;             // 下次可启动探测的时刻
    uint64_t              last_large_ack_ms;              // 最近大包确认时刻
    uint64_t              last_large_loss_ms;             // 最近大包丢失时刻
    uint64_t              blackhole_cooldown_until_ms;    // 黑洞恢复冷却截止时刻
    uint64_t              in_flight_probe_deadline_ms;    // 当前探测超时截止时刻
    uint64_t              in_flight_probe_packet_number;  // 当前探测包号
    uint32_t              probe_interval_ms;              // 稳定期探测间隔
    uint16_t              mtu_min;                        // 可服务 MTU 下限
    uint16_t              mtu_max;                        // 配置 MTU 上限
    uint16_t              mtu_base;                       // 安全基准 MTU
    uint16_t              probe_step;                     // 阶梯步长
    uint16_t              probe_timeout_ms;               // 单次探测超时
    uint16_t              blackhole_loss_window_ms;       // 黑洞统计窗口
    uint16_t              blackhole_cooldown_ms;          // 黑洞冷却时间
    uint16_t              current_mtu;                    // 当前已确认 MTU
    uint16_t              ceiling_mtu;                    // 当前搜索上界
    uint16_t              search_low_mtu;                 // 二分搜索下界
    uint16_t              search_high_mtu;                // 二分搜索上界
    uint16_t              in_flight_probe_mtu;            // 当前探测 MTU
    uint16_t              retry_probe_mtu;                // 待重试探测 MTU
    uint8_t               blackhole_loss_threshold;       // 黑洞连续丢失阈值
    uint8_t               large_loss_streak;              // 连续大包丢失次数
    uint8_t               probe_retries;                  // 额外探测重试次数
    uint8_t               probe_retry_count;              // 当前探测已重试次数
    uint8_t               family;                         // 当前路径地址族
    bool                  enabled;                        // 是否启用探测
    bool                  has_in_flight_probe;            // 是否有等待确认的探测包
    bool                  retry_pending;                  // 是否待重新压入探测包
    utp_mtu_probe_phase_t probe_phase;                    // 阶梯、二分或黑洞恢复阶段
} utp_mtu_discovery_t;

/** @brief 用配置与地址族初始化路径 MTU 发现状态。 */
void     utp_mtu_discovery_init(utp_mtu_discovery_t* discovery, const utp_mtu_config_t* config, uint8_t family);
/** @brief 在路径验证成功后启动或重启 MTU 探测。 */
void     utp_mtu_discovery_on_path_validated(utp_mtu_discovery_t* discovery, uint64_t now_ms);
/** @brief 更新地址族并按该地址族的最小 MTU 重新归一化状态。 */
void     utp_mtu_discovery_set_address_family(utp_mtu_discovery_t* discovery, uint8_t family);
/** @brief 判断该路径是否启用 MTU 探测。 */
bool     utp_mtu_discovery_enabled(const utp_mtu_discovery_t* discovery);
/** @brief 判断是否存在等待确认的 MTU 探测包。 */
bool     utp_mtu_discovery_has_in_flight_probe(const utp_mtu_discovery_t* discovery);
/** @brief 返回当前已确认的路径 MTU。 */
uint16_t utp_mtu_discovery_path_mtu(const utp_mtu_discovery_t* discovery);
/** @brief 返回下一次候选探测 MTU，零表示当前无需探测。 */
uint16_t utp_mtu_discovery_next_probe_mtu(const utp_mtu_discovery_t* discovery);
/** @brief 返回当前允许的数据包最大尺寸。 */
uint16_t utp_mtu_discovery_current_max_packet_size(const utp_mtu_discovery_t* discovery);
/** @brief 返回配置允许的绝对最大数据包尺寸。 */
uint16_t utp_mtu_discovery_absolute_max_packet_size(const utp_mtu_discovery_t* discovery);
/** @brief 判断给定时间是否应发送新的 MTU 探测。 */
bool     utp_mtu_discovery_should_probe(const utp_mtu_discovery_t* discovery, uint64_t now_ms);
/** @brief 记录探测包已发送，并绑定其包号和探测尺寸。 */
bool     utp_mtu_discovery_on_probe_sent(utp_mtu_discovery_t* discovery, uint64_t packet_number, uint16_t probe_mtu,
                                         uint64_t now_ms);
/** @brief 处理探测包确认并推进阶梯或二分搜索。 */
bool     utp_mtu_discovery_on_probe_ack(utp_mtu_discovery_t* discovery, uint64_t packet_number, uint64_t now_ms);
/** @brief 处理探测包丢失并决定重试或收缩搜索上界。 */
bool     utp_mtu_discovery_on_probe_lost(utp_mtu_discovery_t* discovery, uint64_t packet_number, uint64_t now_ms);
/** @brief 处理本地发送探测失败，例如网卡拒绝超大 UDP 包。 */
bool     utp_mtu_discovery_on_probe_send_failed(utp_mtu_discovery_t* discovery, uint16_t probe_mtu, uint64_t now_ms);
/** @brief 处理等待 ACK 的探测包超时。 */
bool     utp_mtu_discovery_on_probe_timeout(utp_mtu_discovery_t* discovery, uint64_t now_ms);
/** @brief 记录大数据包确认，用于黑洞探测恢复。 */
bool     utp_mtu_discovery_on_data_packet_ack(utp_mtu_discovery_t* discovery, uint16_t packet_size, uint64_t now_ms);
/** @brief 记录大数据包丢失，并在阈值达到时进入黑洞恢复。 */
bool     utp_mtu_discovery_on_data_packet_loss(utp_mtu_discovery_t* discovery, uint16_t packet_size, uint64_t now_ms);
/** @brief 返回地址族可服务的最小 MTU。 */
uint16_t utp_mtu_minimum_supported(uint8_t family);
/** @brief 将 MTU 限制到地址族下限与实现上限之间。 */
uint16_t utp_mtu_normalize(uint16_t mtu, uint8_t family);
/** @brief 从 IP MTU 换算为可承载 UTP 包的字节数。 */
uint16_t utp_mtu_packet_size_from_mtu(uint16_t mtu, uint8_t family);
/** @brief 从 UTP 包尺寸反推对应的 IP MTU。 */
uint16_t utp_mtu_from_packet_size(uint16_t packet_size, uint8_t family);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_INTERNAL_MTU_H
