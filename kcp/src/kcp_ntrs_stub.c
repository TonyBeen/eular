#include "kcpp.h"

#include <string.h>

#include "kcp_error.h"

void kcp_ntrs_config_init(kcp_ntrs_config_t *config)
{
    if (config == NULL) {
        return;
    }

    memset(config, 0, sizeof(*config));
    config->node_port = 19000;
    config->connect_timeout_ms = 3000;
    config->detect_rounds = 3;
    config->detect_retries = 9;
}

int32_t kcp_ntrs_configure(struct KcpContext *kcp_ctx, const kcp_ntrs_config_t *config)
{
    (void)kcp_ctx;
    (void)config;
    return NOT_SUPPORT;
}

int32_t kcp_ntrs_start(struct KcpContext *kcp_ctx,
                       on_kcp_ntrs_ready_t ready_cb,
                       on_kcp_ntrs_signal_t signal_cb,
                       void *user_data)
{
    (void)kcp_ctx;
    (void)signal_cb;
    if (ready_cb != NULL) {
        ready_cb(kcp_ctx, NOT_SUPPORT, NULL, user_data);
    }
    return NOT_SUPPORT;
}

int32_t kcp_ntrs_create_session(struct KcpContext *kcp_ctx,
                                const char *dst_peer_id,
                                uint32_t timeout_ms,
                                on_kcp_connected_t cb)
{
    (void)kcp_ctx;
    (void)dst_peer_id;
    (void)timeout_ms;
    if (cb != NULL) {
        cb(NULL, NOT_SUPPORT);
    }
    return NOT_SUPPORT;
}

void kcp_ntrs_stop(struct KcpContext *kcp_ctx)
{
    (void)kcp_ctx;
}
