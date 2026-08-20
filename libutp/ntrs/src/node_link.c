#include <string.h>

#include <ntrs/service.h>

static int32_t
utp_ntrs_link_key_compare(const uint8_t left_node_id[UTP_NTRS_NODE_ID_SIZE],
                          const uint8_t left_nonce[UTP_NTRS_LINK_NONCE_SIZE],
                          const uint8_t right_node_id[UTP_NTRS_NODE_ID_SIZE],
                          const uint8_t right_nonce[UTP_NTRS_LINK_NONCE_SIZE]) {
  const int32_t node_result =
      memcmp(left_node_id, right_node_id, UTP_NTRS_NODE_ID_SIZE);

  return node_result != 0
             ? node_result
             : memcmp(left_nonce, right_nonce, UTP_NTRS_LINK_NONCE_SIZE);
}

bool utp_ntrs_node_link_consider(
    utp_ntrs_node_link_t *link, const utp_ntrs_node_instance_t *remote,
    const uint8_t initiator_node_id[UTP_NTRS_NODE_ID_SIZE],
    const uint8_t initiator_nonce[UTP_NTRS_LINK_NONCE_SIZE], bool *replaced) {
  if (!link->active || !utp_ntrs_node_instance_equal(&link->remote, remote)) {
    const bool had_link = link->active;

    link->remote = *remote;
    (void)memcpy(link->initiator_node_id, initiator_node_id,
                 sizeof(link->initiator_node_id));
    (void)memcpy(link->initiator_nonce, initiator_nonce,
                 sizeof(link->initiator_nonce));
    link->active = true;
    if (replaced != NULL) {
      *replaced = had_link;
    }
    return true;
  }
  if (utp_ntrs_link_key_compare(initiator_node_id, initiator_nonce,
                                link->initiator_node_id,
                                link->initiator_nonce) >= 0) {
    if (replaced != NULL) {
      *replaced = false;
    }
    return false;
  }
  (void)memcpy(link->initiator_node_id, initiator_node_id,
               sizeof(link->initiator_node_id));
  (void)memcpy(link->initiator_nonce, initiator_nonce,
               sizeof(link->initiator_nonce));
  if (replaced != NULL) {
    *replaced = true;
  }
  return true;
}
