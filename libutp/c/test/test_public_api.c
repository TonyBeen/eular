#include <assert.h>
#include <string.h>
#include <utp/utp.h>

int main(void) {
    utp_context_t *context = NULL;

    (void)context;
    assert(strcmp(utp_version(), UTP_VERSION_STRING) == 0);
    assert(strcmp(utp_status_string(UTP_STATUS_PROTOCOL), "protocol") == 0);
    return 0;
}
