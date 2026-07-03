/*************************************************************************
    > File Name: util.h
    > Author: eular
    > Brief:
    > Created Time: Tue 10 Feb 2026 04:44:46 PM CST
 ************************************************************************/

#ifndef __UTP_UTIL_H__
#define __UTP_UTIL_H__

#include <cstddef>

#include "utp/types.h"

namespace eular {
namespace utp {
class Util {
public:
    void RandomBytes(void *buf, size_t len);

    /**
     * @brief 本地生成CID, CID由四部分组成：key、localHost、localPort、timestamp
     *
     * @param key 固定16字节
     * @param localHost 本地IP地址字符串
     * @param localPort 本地端口号
     * @param timestamp 时间戳
     * @param attempt 生成CID的尝试次数, 用于增加CID的随机性
     * @return uint32_t 生成的CID
     */
    static uint32_t GenerateLocalConnectionId(const void *key, const char *localHost, uint16_t localPort, utp_time_t timestamp, uint8_t attempt = 0);

    /**
     * @brief 生成远端CID, 远端CID由四部分组成：key、peerIp、peerCid、timestamp
     *
     * @param key 固定16字节, 与本地CID的key相同
     * @param peerIp 远程IP地址字符串
     * @param peerCid 远端CID
     * @param timestamp 时间戳
     * @param attempt 生成CID的尝试次数, 用于增加CID的随机性
     * @return uint32_t 生成的远端CID
     */
    static uint32_t GenerateRemoteConnectionId(const void *key, const char *peerIp, uint32_t peerCid, utp_time_t timestamp, uint8_t attempt = 0);
};

} // namespace utp
} // namespace eular

#endif // __UTP_UTIL_H__
