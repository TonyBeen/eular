#include <assert.h>
#include <string.h>
#include <utp/utp.h>

int main(void) {
  utp_context_t *context = NULL;
  utp_log_event_t event = {0};

  (void)context;
  assert(strcmp(utp_version(), UTP_VERSION_STRING) == 0);
  event.status = UTP_STATUS_PROTOCOL;
  assert(strcmp(utp_status_string(event.status), "protocol") == 0);
  return 0;
}
