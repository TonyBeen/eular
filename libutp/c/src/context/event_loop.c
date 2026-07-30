#include "context/event_loop.h"

#include <event2/event.h>
#include <string.h>

#define UTP_EVENT_MAX_TIMER_DELAY_US (UINT64_C(4294967295) * UINT64_C(1000000))

static bool utp_event_timer_timeout(uint64_t delay_us, struct timeval *timeout) {
    if (timeout == NULL || delay_us > UTP_EVENT_MAX_TIMER_DELAY_US) {
        return false;
    }
    timeout->tv_sec = (time_t)(delay_us / UINT64_C(1000000));
#if defined(_WIN32)
    timeout->tv_usec = (long)(delay_us % UINT64_C(1000000));
#else
    timeout->tv_usec = (suseconds_t)(delay_us % UINT64_C(1000000));
#endif
    return true;
}

static void utp_event_native_callback(evutil_socket_t file_descriptor, short native_events, void *user_data) {
    utp_event_t *event  = user_data;
    uint32_t     events = 0u;

    (void)file_descriptor;
    if ((native_events & EV_READ) != 0) {
        events |= UTP_EVENT_READABLE;
    }
    if ((native_events & EV_WRITE) != 0) {
        events |= UTP_EVENT_WRITABLE;
    }
    if ((native_events & EV_TIMEOUT) != 0) {
        events |= UTP_EVENT_TIMEOUT;
    }
    if (events != 0u && event != NULL && event->callback != NULL) {
        event->callback(events, event->user_data);
    }
}

utp_internal_error_t utp_event_loop_init(utp_event_loop_t *loop, struct event_base *native_base,
                                         const utp_logger_t *logger, const utp_log_tag_t *tag) {
    if (loop == NULL || native_base == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    loop->native_base = native_base;
    loop->logger      = logger;
    loop->tag         = tag;
    return UTP_INTERNAL_ERROR_OK;
}

void utp_event_loop_close(utp_event_loop_t *loop) {
    if (loop != NULL) {
        loop->native_base = NULL;
    }
}

utp_internal_error_t utp_event_loop_run_once(utp_event_loop_t *loop, bool nonblocking) {
    int flags = EVLOOP_ONCE;

    if (loop == NULL || loop->native_base == NULL) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if (nonblocking) {
        flags |= EVLOOP_NONBLOCK;
    }
    if (event_base_loop(loop->native_base, flags) < 0) {
        utp_internal_log_error(loop->logger, loop->tag, UTP_INTERNAL_ERROR_IO, "event base dispatch failed");
        return UTP_INTERNAL_ERROR_IO;
    }
    return UTP_INTERNAL_ERROR_OK;
}

void utp_event_init(utp_event_t *event) {
    if (event != NULL) {
        memset(event, 0, sizeof(*event));
    }
}

void utp_event_remove(utp_event_t *event) {
    if (event != NULL && event->initialized && event->active) {
        (void)event_del(&event->native_event);
        event->active = false;
    }
}

utp_internal_error_t utp_event_add_udp(utp_event_loop_t *loop, utp_event_t *event, const utp_udp_socket_t *udp_socket,
                                       uint32_t events, bool persistent, utp_event_callback_fn callback,
                                       void *user_data) {
    short native_events = 0;

    if (loop == NULL || loop->native_base == NULL || event == NULL || !utp_udp_socket_is_open(udp_socket) ||
        callback == NULL || events == 0u || (events & ~(UTP_EVENT_READABLE | UTP_EVENT_WRITABLE)) != 0u ||
        event->active) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    if ((events & UTP_EVENT_READABLE) != 0u) {
        native_events |= EV_READ;
    }
    if ((events & UTP_EVENT_WRITABLE) != 0u) {
        native_events |= EV_WRITE;
    }
    if (persistent) {
        native_events |= EV_PERSIST;
    }
    event->callback  = callback;
    event->user_data = user_data;
    if (event_assign(&event->native_event, loop->native_base, (evutil_socket_t)udp_socket->native_handle, native_events,
                     utp_event_native_callback, event) != 0 ||
        event_add(&event->native_event, NULL) != 0) {
        event->callback  = NULL;
        event->user_data = NULL;
        utp_internal_log_error(loop->logger, loop->tag, UTP_INTERNAL_ERROR_IO, "failed to register UDP event");
        return UTP_INTERNAL_ERROR_IO;
    }
    event->initialized = true;
    event->active      = true;
    event->timer       = false;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_event_add_timer(utp_event_loop_t *loop, utp_event_t *event, uint64_t delay_us, bool persistent,
                                         utp_event_callback_fn callback, void *user_data) {
    struct timeval timeout;
    short          native_events = persistent ? EV_PERSIST : 0;

    if (loop == NULL || loop->native_base == NULL || event == NULL || callback == NULL || event->active ||
        (persistent && delay_us == 0u) || !utp_event_timer_timeout(delay_us, &timeout)) {
        return UTP_INTERNAL_ERROR_INVALID_ARGUMENT;
    }
    event->callback  = callback;
    event->user_data = user_data;
    if (event_assign(&event->native_event, loop->native_base, (evutil_socket_t)-1, native_events,
                     utp_event_native_callback, event) != 0 ||
        event_add(&event->native_event, &timeout) != 0) {
        event->callback  = NULL;
        event->user_data = NULL;
        utp_internal_log_error(loop->logger, loop->tag, UTP_INTERNAL_ERROR_IO, "failed to register timer event");
        return UTP_INTERNAL_ERROR_IO;
    }
    event->initialized = true;
    event->active      = true;
    event->timer       = true;
    return UTP_INTERNAL_ERROR_OK;
}

utp_internal_error_t utp_event_reset_timer(utp_event_t *event, uint64_t delay_us) {
    struct timeval timeout;

    if (event == NULL || !event->initialized || !event->active || !event->timer ||
        !utp_event_timer_timeout(delay_us, &timeout)) {
        return UTP_INTERNAL_ERROR_STATE;
    }
    return event_add(&event->native_event, &timeout) == 0 ? UTP_INTERNAL_ERROR_OK : UTP_INTERNAL_ERROR_IO;
}
