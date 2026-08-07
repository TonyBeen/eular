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
    uint32_t timestamp_seconds;
    uint32_t cid;
    uint32_t version;
    uint32_t secret;
    uint16_t family;
    uint8_t  token_type;
    uint8_t  encryption_mode;
    uint8_t  address[16];
} utp_token_meta_t;

typedef struct utp_token_auth {
    uint8_t  key[UTP_TOKEN_KEY_SIZE];
    uint8_t  old_key[UTP_TOKEN_KEY_SIZE];
    uint64_t key_updated_seconds;
    bool     initialized;
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
