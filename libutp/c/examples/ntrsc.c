#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#include <event2/event.h>
#include <utp/utp.h>

typedef struct ntrsc_app {
    struct event_base* base;  // 调用方持有的 libevent loop
#if !defined(_WIN32)
    struct event* signal_int;   // SIGINT 退出事件
    struct event* signal_term;  // SIGTERM 退出事件
#endif
    utp_context_t* context;    // NAT 探测使用的 Context
    bool           completed;  // 是否收到 NAT 探测完成回调
    int32_t        exit_code;  // 进程最终退出码
} ntrsc_app_t;

static void ntrsc_usage(const char* program)
{
    (void)fprintf(stderr,
                  "Usage: %s --nat-address IP --nat-port PORT [--bind-address IP] [--bind-port PORT]\n"
                  "          [--interface NAME] [--phase-timeout-ms MS] [--log-level LEVEL]\n"
                  "\n"
                  "  --nat-address IP       NAT Node probe endpoint literal IP (required)\n"
                  "  --nat-port PORT        NAT Node probe UDP port (required)\n"
                  "  --bind-address IP      Local bind IP; defaults to 0.0.0.0 or :: by NAT address family\n"
                  "  --bind-port PORT       Local UDP port; defaults to 0\n"
                  "  --interface NAME       Bind the UDP socket to a network interface\n"
                  "  --phase-timeout-ms MS  Per-phase probe timeout; defaults to 3000\n"
                  "  --log-level LEVEL      debug, info, warning, error, or silence (default)\n",
                  program);
}

static bool ntrsc_parse_u16(const char* text, uint16_t* out_value)
{
    char*         end = NULL;
    unsigned long value;

    if (text == NULL || out_value == NULL || *text == '\0') {
        return false;
    }
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > UINT16_MAX) {
        return false;
    }
    *out_value = (uint16_t)value;
    return true;
}

static bool ntrsc_parse_u32(const char* text, uint32_t* out_value)
{
    char*              end = NULL;
    unsigned long long value;

    if (text == NULL || out_value == NULL || *text == '\0') {
        return false;
    }
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > UINT32_MAX) {
        return false;
    }
    *out_value = (uint32_t)value;
    return true;
}

static bool ntrsc_parse_log_level(const char* text, utp_log_level_t* out_level)
{
    if (strcmp(text, "debug") == 0) {
        *out_level = UTP_LOG_LEVEL_DEBUG;
        return true;
    }
    if (strcmp(text, "info") == 0) {
        *out_level = UTP_LOG_LEVEL_INFO;
        return true;
    }
    if (strcmp(text, "warning") == 0) {
        *out_level = UTP_LOG_LEVEL_WARNING;
        return true;
    }
    if (strcmp(text, "error") == 0) {
        *out_level = UTP_LOG_LEVEL_ERROR;
        return true;
    }
    if (strcmp(text, "silence") == 0) {
        *out_level = UTP_LOG_LEVEL_SILENCE;
        return true;
    }
    return false;
}

static const char* ntrsc_log_level_name(utp_log_level_t level)
{
    switch (level) {
    case UTP_LOG_LEVEL_DEBUG:
        return "debug";
    case UTP_LOG_LEVEL_INFO:
        return "info";
    case UTP_LOG_LEVEL_WARNING:
        return "warning";
    case UTP_LOG_LEVEL_ERROR:
        return "error";
    case UTP_LOG_LEVEL_SILENCE:
        return "silence";
    default:
        return "unknown";
    }
}

static void ntrsc_log_sink(utp_log_level_t level, const char* message)
{
    (void)fprintf(stderr, "ntrsc event=libutp_log level=%s message=%s\n", ntrsc_log_level_name(level), message);
}

static const char* ntrsc_nat_class_name(utp_nat_class_t nat_class)
{
    switch (nat_class) {
    case UTP_NAT_CLASS_UNKNOWN:
        return "unknown";
    case UTP_NAT_CLASS_OPEN_PUBLIC:
        return "open_public";
    case UTP_NAT_CLASS_OPEN_PUBLIC_WITH_FIREWALL:
        return "open_public_with_firewall";
    case UTP_NAT_CLASS_FULL_CONE:
        return "full_cone";
    case UTP_NAT_CLASS_IP_RESTRICTED:
        return "ip_restricted";
    case UTP_NAT_CLASS_PORT_RESTRICTED:
        return "port_restricted";
    case UTP_NAT_CLASS_SYMMETRIC:
        return "symmetric";
    case UTP_NAT_CLASS_SYMMETRIC_MULTI_LINE:
        return "symmetric_multi_line";
    case UTP_NAT_CLASS_UDP_BLOCKED:
        return "udp_blocked";
    default:
        return "invalid";
    }
}

static const char* ntrsc_family_name(uint8_t family)
{
    if (family == 4u || family == (uint8_t)AF_INET) {
        return "ipv4";
    }
    if (family == 6u || family == (uint8_t)AF_INET6) {
        return "ipv6";
    }
    return "unknown";
}

static const char* ntrsc_endpoint_format(const utp_endpoint_t* endpoint, char output[80])
{
    char address[INET6_ADDRSTRLEN];

    if ((endpoint->family == 4u || endpoint->family == (uint8_t)AF_INET) &&
        inet_ntop(AF_INET, endpoint->address, address, (socklen_t)sizeof(address)) != NULL) {
        (void)snprintf(output, 80u, "%s:%" PRIu16, address, endpoint->port);
        return output;
    }
    if ((endpoint->family == 6u || endpoint->family == (uint8_t)AF_INET6) &&
        inet_ntop(AF_INET6, endpoint->address, address, (socklen_t)sizeof(address)) != NULL) {
        if (endpoint->scope_id != 0u) {
            (void)snprintf(output, 80u, "[%s%%%" PRIu32 "]:%" PRIu16, address, endpoint->scope_id, endpoint->port);
        } else {
            (void)snprintf(output, 80u, "[%s]:%" PRIu16, address, endpoint->port);
        }
        return output;
    }
    (void)snprintf(output, 80u, "<none>");
    return output;
}

static void ntrsc_print_port_samples(const utp_nat_probe_result_t* result)
{
    uint8_t index;

    (void)fputs("ntrsc event=port_samples values=", stdout);
    if (result->port_sample_count == 0u) {
        (void)fputs("none", stdout);
    }
    for (index = 0u; index < result->port_sample_count; ++index) {
        (void)fprintf(stdout, "%s%" PRIu16, index == 0u ? "" : ",", result->port_samples[index]);
    }
    (void)fputc('\n', stdout);
}

static void ntrsc_on_probe_complete(utp_context_t* context, utp_status_t status, const utp_nat_probe_result_t* result,
                                    void* user_data)
{
    ntrsc_app_t* app = user_data;

    (void)context;
    app->completed = true;
    if (status != UTP_STATUS_OK || result == NULL) {
        app->exit_code = EXIT_FAILURE;
        (void)fprintf(stderr, "ntrsc event=probe_failed status=%s\n", utp_status_string(status));
        event_base_loopbreak(app->base);
        return;
    }
    {
        char primary[80];
        char secondary[80];

        (void)fprintf(stdout,
                      "ntrsc event=probe_complete status=ok class=%s family=%s primary_mapped=%s "
                      "secondary_mapped=%s primary_rtt_ms=%" PRId32 " secondary_rtt_ms=%" PRId32
                      " "
                      "probe_time_us=%" PRIu64 " expires_at_us=%" PRIu64 "\n",
                      ntrsc_nat_class_name(result->nat_class), ntrsc_family_name(result->address_family),
                      ntrsc_endpoint_format(&result->primary_mapped_endpoint, primary),
                      ntrsc_endpoint_format(&result->secondary_mapped_endpoint, secondary), result->primary_rtt_ms,
                      result->secondary_rtt_ms, result->probe_time_us, result->expires_at_us);
    }
    ntrsc_print_port_samples(result);
    app->exit_code = EXIT_SUCCESS;
    event_base_loopbreak(app->base);
}

#if !defined(_WIN32)
static void ntrsc_on_signal(evutil_socket_t fd, int16_t events, void* user_data)
{
    ntrsc_app_t* app = user_data;

    (void)fd;
    (void)events;
    if (!app->completed) {
        (void)utp_context_cancel_nat_probe(app->context);
        (void)fprintf(stderr, "ntrsc event=probe_cancelled reason=signal\n");
    }
    app->exit_code = EXIT_FAILURE;
    event_base_loopbreak(app->base);
}
#endif

static bool ntrsc_address_is_ipv6(const char* address)
{
    struct in6_addr parsed;

    return inet_pton(AF_INET6, address, &parsed) == 1;
}

int main(int argc, char** argv)
{
    ntrsc_app_t             app             = {0};
    utp_context_options_t   context_options = UTP_CONTEXT_OPTIONS_INIT;
    utp_nat_probe_options_t probe_options   = UTP_NAT_PROBE_OPTIONS_INIT;
    const char*             nat_address     = NULL;
    const char*             bind_address    = NULL;
    const char*             interface_name  = NULL;
    uint16_t                nat_port        = 0u;
    uint16_t                bind_port       = 0u;
    uint16_t                local_port      = 0u;
    utp_log_level_t         log_level       = UTP_LOG_LEVEL_SILENCE;
    int32_t                 index;
    utp_status_t            status;
#if defined(_WIN32)
    WSADATA winsock_data;
    bool    winsock_started = false;
#endif

    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        ntrsc_usage(argv[0]);
        return EXIT_SUCCESS;
    }
    for (index = 1; index < argc; index += 2) {
        if (index + 1 >= argc) {
            ntrsc_usage(argv[0]);
            return EXIT_FAILURE;
        }
        if (strcmp(argv[index], "--nat-address") == 0) {
            nat_address = argv[index + 1];
        } else if (strcmp(argv[index], "--nat-port") == 0) {
            if (!ntrsc_parse_u16(argv[index + 1], &nat_port) || nat_port == 0u) {
                ntrsc_usage(argv[0]);
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[index], "--bind-address") == 0) {
            bind_address = argv[index + 1];
        } else if (strcmp(argv[index], "--bind-port") == 0) {
            if (!ntrsc_parse_u16(argv[index + 1], &bind_port)) {
                ntrsc_usage(argv[0]);
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[index], "--interface") == 0) {
            interface_name = argv[index + 1];
        } else if (strcmp(argv[index], "--phase-timeout-ms") == 0) {
            if (!ntrsc_parse_u32(argv[index + 1], &probe_options.phase_timeout_ms)) {
                ntrsc_usage(argv[0]);
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[index], "--log-level") == 0) {
            if (!ntrsc_parse_log_level(argv[index + 1], &log_level)) {
                ntrsc_usage(argv[0]);
                return EXIT_FAILURE;
            }
        } else {
            ntrsc_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (nat_address == NULL || nat_port == 0u) {
        ntrsc_usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (bind_address == NULL) {
        bind_address = ntrsc_address_is_ipv6(nat_address) ? "::" : "0.0.0.0";
    }
#if defined(_WIN32)
    if (WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0) {
        (void)fprintf(stderr, "ntrsc event=winsock_start_failed\n");
        return EXIT_FAILURE;
    }
    winsock_started = true;
#endif
    app.exit_code = EXIT_FAILURE;
    app.base      = event_base_new();
    if (app.base == NULL) {
        (void)fprintf(stderr, "ntrsc event=event_base_create_failed\n");
        goto cleanup;
    }
    context_options.event_base = app.base;
    context_options.log_sink   = log_level == UTP_LOG_LEVEL_SILENCE ? NULL : ntrsc_log_sink;
    context_options.log_level  = log_level;
    status                     = utp_context_create(&context_options, &app.context);
    if (status != UTP_STATUS_OK) {
        (void)fprintf(stderr, "ntrsc event=context_create_failed status=%s\n", utp_status_string(status));
        goto cleanup;
    }
    status = utp_context_bind(app.context, bind_address, bind_port, interface_name, &local_port);
    if (status != UTP_STATUS_OK) {
        (void)fprintf(stderr, "ntrsc event=context_bind_failed address=%s port=%" PRIu16 " status=%s\n", bind_address,
                      bind_port, utp_status_string(status));
        goto cleanup;
    }
    probe_options.nat_service_address = nat_address;
    probe_options.nat_service_port    = nat_port;
#if !defined(_WIN32)
    app.signal_int  = evsignal_new(app.base, SIGINT, ntrsc_on_signal, &app);
    app.signal_term = evsignal_new(app.base, SIGTERM, ntrsc_on_signal, &app);
    if (app.signal_int == NULL || app.signal_term == NULL || event_add(app.signal_int, NULL) != 0 ||
        event_add(app.signal_term, NULL) != 0) {
        (void)fprintf(stderr, "ntrsc event=signal_event_create_failed\n");
        goto cleanup;
    }
#endif
    status = utp_context_probe_nat(app.context, &probe_options, ntrsc_on_probe_complete, &app);
    if (status != UTP_STATUS_OK) {
        (void)fprintf(stderr, "ntrsc event=probe_start_failed status=%s\n", utp_status_string(status));
        goto cleanup;
    }
    (void)fprintf(stdout,
                  "ntrsc event=probe_started bind_address=%s bind_port=%" PRIu16 " nat_address=%s nat_port=%" PRIu16
                  " phase_timeout_ms=%" PRIu32 "\n",
                  bind_address, local_port, nat_address, nat_port, probe_options.phase_timeout_ms);
    (void)event_base_dispatch(app.base);

cleanup:
#if !defined(_WIN32)
    if (app.signal_int != NULL) {
        event_free(app.signal_int);
    }
    if (app.signal_term != NULL) {
        event_free(app.signal_term);
    }
#endif
    if (app.context != NULL) {
        utp_context_destroy(app.context);
    }
    if (app.base != NULL) {
        event_base_free(app.base);
    }
#if defined(_WIN32)
    if (winsock_started) {
        (void)WSACleanup();
    }
#endif
    return app.exit_code;
}
