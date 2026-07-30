#ifndef EULAR_UTP_CONTEXT_CONTEXT_H
#define EULAR_UTP_CONTEXT_CONTEXT_H

#include <utp/utp.h>

#include "context/event_loop.h"
#include "util/log.h"

struct utp_context {
    utp_event_loop_t event_loop;
    utp_logger_t     logger;
    utp_log_tag_t    tag;
};

#endif  // EULAR_UTP_CONTEXT_CONTEXT_H
