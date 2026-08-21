#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200112L
#endif

#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <cinttypes>
#include <cstdarg>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>

#include <arpa/inet.h>
#if defined(__APPLE__)
#include <pthread.h>
#elif defined(__linux__)
#include <unistd.h>

#include <sys/syscall.h>
#endif
#endif

#include <event2/event.h>
#include <event2/util.h>
#include <utils/CLI11.hpp>
#include <utp/utp.h>

static uint64_t ntrsc_kernel_thread_id()
{
#if defined(_WIN32)
    return static_cast<uint64_t>(GetCurrentThreadId());
#elif defined(__APPLE__)
    uint64_t thread_id = 0u;

    (void)pthread_threadid_np(nullptr, &thread_id);
    return thread_id;
#elif defined(__linux__)
    return static_cast<uint64_t>(syscall(SYS_gettid));
#else
    return 0u;
#endif
}

static int32_t ntrsc_log_printf(FILE* stream, const char* source, int32_t line, const char* format, ...)
{
    char        message[1024];
    std::string content;
    va_list     arguments;
    const auto  now     = std::chrono::system_clock::now();
    const auto  seconds = std::chrono::system_clock::to_time_t(now);
    const auto  milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm     local_time = {};
    const char* level      = "I";
    const char* file_name;
    int32_t     written;

    va_start(arguments, format);
    written = std::vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (written < 0) return written;
    message[std::strcspn(message, "\r\n")] = '\0';
    content                                = message;
    if (content.compare(0u, 12u, "ntrsc event=") == 0) content.erase(0u, 12u);
    if (content.find("level=debug") != std::string::npos)
        level = "D";
    else if (content.find("level=warning") != std::string::npos)
        level = "W";
    else if (content.find("level=error") != std::string::npos || content.find("failed") != std::string::npos)
        level = "E";
    const std::string::size_type message_offset = content.find("message=");
    if (message_offset != std::string::npos) content.erase(0u, message_offset + 8u);
#if defined(_WIN32)
    (void)localtime_s(&local_time, &seconds);
#else
    (void)localtime_r(&seconds, &local_time);
#endif
    file_name = std::strrchr(source, '/');
    return static_cast<int32_t>(std::fprintf(
        stream, "%04d-%02d-%02d %02d:%02d:%02d.%03lld %5" PRIu64 " %s ntrsc: %s    %s:%" PRId32 "\n",
        local_time.tm_year + 1900, local_time.tm_mon + 1, local_time.tm_mday, local_time.tm_hour, local_time.tm_min,
        local_time.tm_sec, static_cast<long long>(milliseconds), ntrsc_kernel_thread_id(), level, content.c_str(),
        file_name == nullptr ? source : file_name + 1, line));
}

#define fprintf(stream, ...) ntrsc_log_printf(stream, __FILE__, __LINE__, __VA_ARGS__)

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
    char    values[UTP_NAT_PORT_SAMPLE_CAPACITY * 6u + 1u] = {0};
    size_t  offset                                         = 0u;
    uint8_t index;

    if (result->port_sample_count == 0u) {
        (void)fprintf(stdout, "port samples: none\n");
        return;
    }
    for (index = 0u; index < result->port_sample_count; ++index) {
        const int32_t written = snprintf(values + offset, sizeof(values) - offset, "%s%" PRIu16, index == 0u ? "" : ",",
                                         result->port_samples[index]);

        if (written < 0 || (size_t)written >= sizeof(values) - offset) {
            return;
        }
        offset += (size_t)written;
    }
    (void)fprintf(stdout, "port samples: %s\n", values);
}

static void ntrsc_on_probe_complete(utp_context_t* context, utp_status_t status, const utp_nat_probe_result_t* result,
                                    void* user_data)
{
    ntrsc_app_t* app = static_cast<ntrsc_app_t*>(user_data);

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
    ntrsc_app_t* app = static_cast<ntrsc_app_t*>(user_data);

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

/** @brief 将 NAT 服务主机名解析为一个数字 IP；双栈名称优先使用 IPv4。 */
static bool ntrsc_resolve_nat_address(const char* input, char output[INET6_ADDRSTRLEN])
{
    struct evutil_addrinfo  hints     = {};
    struct evutil_addrinfo* addresses = NULL;
    struct evutil_addrinfo* current;

    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    if (evutil_getaddrinfo(input, NULL, &hints, &addresses) != 0) {
        return false;
    }
    for (current = addresses; current != NULL; current = current->ai_next) {
        if (current->ai_family == AF_INET &&
            inet_ntop(AF_INET, &((const struct sockaddr_in*)current->ai_addr)->sin_addr, output, INET6_ADDRSTRLEN) !=
                NULL) {
            evutil_freeaddrinfo(addresses);
            return true;
        }
    }
    for (current = addresses; current != NULL; current = current->ai_next) {
        if (current->ai_family == AF_INET6 &&
            inet_ntop(AF_INET6, &((const struct sockaddr_in6*)current->ai_addr)->sin6_addr, output, INET6_ADDRSTRLEN) !=
                NULL) {
            evutil_freeaddrinfo(addresses);
            return true;
        }
    }
    evutil_freeaddrinfo(addresses);
    return false;
}

static int32_t ntrsc_run(const char* nat_address, uint16_t nat_port, const char* bind_address, uint16_t bind_port,
                         const char* interface_name, uint32_t phase_timeout_ms, bool verbose)
{
    ntrsc_app_t             app             = {};
    utp_context_options_t   context_options = UTP_CONTEXT_OPTIONS_INIT;
    utp_nat_probe_options_t probe_options   = UTP_NAT_PROBE_OPTIONS_INIT;
    char                    nat_numeric_address[INET6_ADDRSTRLEN];
    uint16_t                local_port = 0u;
    utp_status_t            status;
#if defined(_WIN32)
    WSADATA winsock_data;
    bool    winsock_started = false;
#endif

    if (nat_address == NULL || nat_port == 0u) {
        return EXIT_FAILURE;
    }
    probe_options.phase_timeout_ms = phase_timeout_ms;
#if defined(_WIN32)
    if (WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0) {
        (void)fprintf(stderr, "ntrsc event=winsock_start_failed\n");
        return EXIT_FAILURE;
    }
    winsock_started = true;
#endif
    if (!ntrsc_resolve_nat_address(nat_address, nat_numeric_address)) {
        (void)fprintf(stderr, "ntrsc event=nat_address_resolve_failed address=%s\n", nat_address);
        goto cleanup;
    }
    if (bind_address == NULL) {
        bind_address = ntrsc_address_is_ipv6(nat_numeric_address) ? "::" : "0.0.0.0";
    }
    app.exit_code = EXIT_FAILURE;
    app.base      = event_base_new();
    if (app.base == NULL) {
        (void)fprintf(stderr, "ntrsc event=event_base_create_failed\n");
        goto cleanup;
    }
    context_options.event_base = app.base;
    context_options.log_sink   = verbose ? ntrsc_log_sink : NULL;
    context_options.log_level  = verbose ? UTP_LOG_LEVEL_DEBUG : UTP_LOG_LEVEL_SILENCE;
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
    probe_options.nat_service_address = nat_numeric_address;
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
                  "ntrsc event=probe_started bind_address=%s bind_port=%" PRIu16
                  " nat_address=%s resolved_address=%s nat_port=%" PRIu16 " phase_timeout_ms=%" PRIu32 "\n",
                  bind_address, local_port, nat_address, nat_numeric_address, nat_port,
                  probe_options.phase_timeout_ms == 0u ? 3000u : probe_options.phase_timeout_ms);
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

int main(int argc, char** argv)
{
    CLI::App    cli{"libutp NAT detection client"};
    std::string nat_address;
    uint16_t    nat_port = 0u;
    std::string bind_address;
    uint16_t    bind_port = 0u;
    std::string interface_name;
    uint32_t    phase_timeout_ms = 0u;
    bool        verbose          = false;

    cli.add_option("-a,--nat-address", nat_address, "NAT Node probe hostname or IP")->required();
    cli.add_option("-p,--nat-port", nat_port, "NAT Node probe UDP port")->required();
    cli.add_option("-b,--bind-address", bind_address, "Local bind IP");
    cli.add_option("-P,--bind-port", bind_port, "Local bind UDP port");
    cli.add_option("-i,--interface", interface_name, "Bind UDP socket to this interface");
    cli.add_option("-t,--phase-timeout-ms", phase_timeout_ms, "Per-phase timeout in milliseconds");
    cli.add_flag("-v", verbose, "Print the complete NAT probe process");
    CLI11_PARSE(cli, argc, argv);

    return ntrsc_run(nat_address.c_str(), nat_port, bind_address.empty() ? NULL : bind_address.c_str(), bind_port,
                     interface_name.empty() ? NULL : interface_name.c_str(), phase_timeout_ms, verbose);
}
