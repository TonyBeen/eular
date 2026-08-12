#ifndef EULAR_UTP_INTERNAL_TOKEN_H
#define EULAR_UTP_INTERNAL_TOKEN_H

#include <stdbool.h>
#include <stdint.h>

#include "socket/address.h"
#include "util/error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_TOKEN_KEY_SIZE             32u
#define UTP_TOKEN_NONCE_SIZE           12u
#define UTP_TOKEN_TAG_SIZE             16u
#define UTP_TOKEN_META_SIZE            36u
#define UTP_TOKEN_SIZE                 (UTP_TOKEN_NONCE_SIZE + UTP_TOKEN_META_SIZE + UTP_TOKEN_TAG_SIZE)
#define UTP_TOKEN_KEY_ROTATION_SECONDS UINT64_C(3600)

typedef enum utp_token_type {
    UTP_TOKEN_TYPE_PATH_VALIDATION = 1,
    UTP_TOKEN_TYPE_ZERO_RTT        = 2,
} utp_token_type_t;

typedef struct utp_token_meta {
    uint32_t timestamp_seconds;  // 签发时间，Unix 秒
    uint32_t cid;                // 绑定连接 ID
    uint32_t version;            // 票据格式版本
    uint32_t secret;             // 防伪随机值
    uint16_t family;             // 绑定地址族
    uint8_t  token_type;         // 路径验证或 0-RTT 票据
    uint8_t  encryption_mode;    // 恢复时使用的加密模式
    uint8_t  address[16];        // 绑定对端地址
} utp_token_meta_t;

typedef struct utp_token_auth {
    uint8_t  key[UTP_TOKEN_KEY_SIZE];      // 当前票据认证密钥
    uint8_t  old_key[UTP_TOKEN_KEY_SIZE];  // 上一代轮换密钥
    uint64_t key_updated_seconds;          // 当前密钥启用时刻
    bool     initialized;                  // 是否已生成认证密钥
} utp_token_auth_t;

/** @brief 初始化 Context 私有的票据认证密钥。 */
utp_internal_error_t utp_token_auth_init(utp_token_auth_t* auth, uint64_t now_seconds);
/** @brief 清理票据认证密钥。 */
void                 utp_token_auth_cleanup(utp_token_auth_t* auth);
/** @brief 用当前轮换密钥封装固定长度票据。 */
utp_internal_error_t utp_token_auth_seal(utp_token_auth_t* auth, const utp_token_meta_t* meta,
                                         uint8_t token[UTP_TOKEN_SIZE], uint64_t now_seconds);
/** @brief 用当前或上一代密钥打开并校验指定类型的票据。 */
utp_internal_error_t utp_token_auth_open(utp_token_auth_t* auth, const uint8_t token[UTP_TOKEN_SIZE],
                                         uint8_t expected_type, utp_token_meta_t* out_meta, uint64_t now_seconds);
/** @brief 比较 token 元数据绑定的地址，仅比较地址族和主机地址。 */
bool                 utp_token_meta_address_matches(const utp_token_meta_t* meta, const utp_address_t* address);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_INTERNAL_TOKEN_H
