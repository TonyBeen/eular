#ifndef EULAR_UTP_C_STREAM_H
#define EULAR_UTP_C_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <utp/status.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct utp_stream utp_stream_t;

#define UTP_STREAM_PRIORITY_HIGHEST 0u
#define UTP_STREAM_PRIORITY_LOWEST  7u
#define UTP_STREAM_PRIORITY_DEFAULT 4u

typedef struct utp_stream_read_view {
    const uint8_t* data;
    uint64_t       offset;
    size_t         length;
    bool           fin;
} utp_stream_read_view_t;

typedef struct utp_stream_write_view {
    uint8_t* data;
    size_t   length;
} utp_stream_write_view_t;

uint32_t     utp_stream_id(const utp_stream_t* stream);
utp_status_t utp_stream_write(utp_stream_t* stream, const void* data, size_t length);
utp_status_t utp_stream_read(utp_stream_t* stream, void* buffer, size_t capacity, size_t* out_length);
void         utp_stream_close(utp_stream_t* stream);
utp_status_t utp_stream_reset(utp_stream_t* stream, uint16_t error_code);

utp_status_t utp_stream_acquire_write_views(utp_stream_t* stream, utp_stream_write_view_t* views, size_t view_capacity,
                                            size_t* out_view_count, size_t* out_capacity);
utp_status_t utp_stream_commit_write_views(utp_stream_t* stream, size_t length);
utp_status_t utp_stream_acquire_read_view(utp_stream_t* stream, utp_stream_read_view_t* out_view);
utp_status_t utp_stream_commit_read_view(utp_stream_t* stream, uint64_t offset, size_t length);

size_t       utp_stream_readable_bytes(const utp_stream_t* stream);
bool         utp_stream_is_closed(const utp_stream_t* stream);
bool         utp_stream_reset_by_peer(const utp_stream_t* stream);
utp_status_t utp_stream_set_priority(utp_stream_t* stream, uint8_t priority);
uint8_t      utp_stream_priority(const utp_stream_t* stream);

#ifdef __cplusplus
}
#endif

#endif  // EULAR_UTP_C_STREAM_H
