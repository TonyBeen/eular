#ifndef EULAR_UTP_INTERNAL_EVENT_LOOP_H
#define EULAR_UTP_INTERNAL_EVENT_LOOP_H

#include <stdbool.h>
#include <stdint.h>

#include <event2/event_struct.h>

#include "socket/udp.h"
#include "util/error.h"
#include "util/log.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UTP_EVENT_READABLE UINT32_C(1)
#define UTP_EVENT_WRITABLE UINT32_C(2)
#define UTP_EVENT_TIMEOUT  UINT32_C(4)

typedef void (*utp_event_callback_fn)(uint32_t events, void* user_data);

typedef struct utp_event_loop {
    struct event_base*   native_base;
    const utp_logger_t*  logger;
    const utp_log_tag_t* tag;
} utp_event_loop_t;

typedef struct utp_event {
    struct event          native_event;
    utp_event_callback_fn callback;
    void*                 user_data;
    bool                  initialized;
    bool                  active;
    bool                  timer;
} utp_event_t;

// Borrows native_base, logger, and tag. The caller retains event_base creation, dispatch, and destruction ownership.
utp_internal_error_t utp_event_loop_init(utp_event_loop_t* loop, struct event_base* native_base,
                                         const utp_logger_t* logger, const utp_log_tag_t* tag);
void                 utp_event_loop_close(utp_event_loop_t* loop);
utp_internal_error_t utp_event_loop_run_once(utp_event_loop_t* loop, bool nonblocking);

void                 utp_event_init(utp_event_t* event);
void                 utp_event_remove(utp_event_t* event);
utp_internal_error_t utp_event_add_udp(utp_event_loop_t* loop, utp_event_t* event, const utp_udp_socket_t* udp_socket,
                                       uint32_t events, bool persistent, utp_event_callback_fn callback,
                                       void* user_data);
utp_internal_error_t utp_event_add_timer(utp_event_loop_t* loop, utp_event_t* event, uint64_t delay_us, bool persistent,
                                         utp_event_callback_fn callback, void* user_data);
utp_internal_error_t utp_event_reset_timer(utp_event_t* event, uint64_t delay_us);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_INTERNAL_EVENT_LOOP_H
