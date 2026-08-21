#ifndef EULAR_NTRS_SERVICE_UTIL_H
#define EULAR_NTRS_SERVICE_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include <ntrs/service.h>

#define UTP_NTRS_NODE_INSTANCE_TEXT_SIZE 66u

/** @brief 解析仅允许数字 IP 的 "IPv4:port" 或 "[IPv6]:port" endpoint。 */
bool utp_ntrs_endpoint_parse(const char *text, utp_ntrs_endpoint_t *endpoint);
/** @brief 解析数字 endpoint 或将 "host:port" 解析为一个数字 endpoint。 */
bool utp_ntrs_endpoint_resolve(const char *text, utp_ntrs_endpoint_t *endpoint);
/** @brief 将 sockaddr 转为 endpoint。 */
bool utp_ntrs_endpoint_from_sockaddr(utp_ntrs_endpoint_t *endpoint,
                                     const struct sockaddr *address,
                                     socklen_t length);
/** @brief 将 socket 限制在指定 Linux 网卡；空指针表示不限制。 */
bool utp_ntrs_socket_bind_interface(int32_t fd, const char *interface_name);
/** @brief 将 endpoint 转为 sockaddr。 */
bool utp_ntrs_endpoint_to_sockaddr(const utp_ntrs_endpoint_t *endpoint,
                                   struct sockaddr_storage *storage,
                                   socklen_t *length);
/** @brief 输出 endpoint 的数字形式。 */
const char *utp_ntrs_endpoint_format(const utp_ntrs_endpoint_t *endpoint,
                                     char *buffer, size_t capacity);
/** @brief 解析固定长度的十六进制字节串。 */
bool utp_ntrs_hex_decode(const char *text, uint8_t *output,
                         size_t output_length);
/** @brief 将 Node 实例输出为 "node_id:boot_id" 十六进制文本。 */
const char *
utp_ntrs_node_instance_format(const utp_ntrs_node_instance_t *instance,
                              char *buffer, size_t capacity);
/** @brief 当前 Unix 单调毫秒时间。 */
uint64_t utp_ntrs_now_ms(void);

#endif // EULAR_NTRS_SERVICE_UTIL_H
