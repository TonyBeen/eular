#include <assert.h>
#include <event2/event.h>
#include <string.h>
#include <utp/utp.h>

int main(void) {
    struct event_base    *event_base = event_base_new();
    utp_context_options_t options    = {event_base, NULL, 7u};
    utp_context_t        *context    = NULL;

    assert(event_base != NULL);
    assert(utp_context_create(&options, &context) == UTP_STATUS_OK);
    assert(context != NULL);
    utp_context_destroy(context);
    event_base_free(event_base);
    assert(strcmp(utp_version(), UTP_VERSION_STRING) == 0);
    assert(strcmp(utp_status_string(UTP_STATUS_PROTOCOL), "protocol") == 0);
    return 0;
}
