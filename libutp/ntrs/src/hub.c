#include <stdlib.h>
#include <string.h>

#include <netinet/in.h>
#include <ntrs/service.h>

#define UTP_NTRS_ASSIGNMENT_EXCLUSION_MS 30000u

typedef struct utp_ntrs_hub_node {
  utp_ntrs_node_registration_t registration; // Node 最近一次注册信息
  utp_ntrs_assignment_t assignments[2];      // IPv4、IPv6 当前下发的主备
  utp_ntrs_node_instance_t
      exclusions[2][UTP_NTRS_MAX_EXCLUDED_NODES]; // Node 主动报告的失效协同实例
  uint64_t
      exclusion_expire_at_ms[2]
                            [UTP_NTRS_MAX_EXCLUDED_NODES]; // 排除项绝对失效时刻
  uint64_t last_heartbeat_ms;                              // 最近注册或心跳时刻
  uint8_t exclusion_count[2]; // 每地址族有效排除项数量
} utp_ntrs_hub_node_t;

static bool utp_ntrs_bytes_nonzero(const uint8_t *data, size_t length) {
  size_t index;

  for (index = 0u; index < length; ++index) {
    if (data[index] != 0u) {
      return true;
    }
  }
  return false;
}

bool utp_ntrs_node_instance_equal(const utp_ntrs_node_instance_t *left,
                                  const utp_ntrs_node_instance_t *right) {
  return memcmp(left, right, sizeof(*left)) == 0;
}

static bool utp_ntrs_node_id_equal(const utp_ntrs_node_instance_t *left,
                                   const utp_ntrs_node_instance_t *right) {
  return memcmp(left->node_id, right->node_id, sizeof(left->node_id)) == 0;
}

static size_t utp_ntrs_address_length(uint8_t family) {
  return family == (uint8_t)AF_INET ? 4u : 16u;
}

static bool utp_ntrs_endpoint_same_ip(const utp_ntrs_endpoint_t *left,
                                      const utp_ntrs_endpoint_t *right) {
  return left->family == right->family &&
         memcmp(left->address, right->address,
                utp_ntrs_address_length(left->family)) == 0;
}

static bool utp_ntrs_endpoint_equal(const utp_ntrs_endpoint_t *left,
                                    const utp_ntrs_endpoint_t *right) {
  return left->family == right->family && left->port == right->port &&
         memcmp(left->address, right->address, sizeof(left->address)) == 0;
}

static bool utp_ntrs_endpoint_shape_valid(const utp_ntrs_endpoint_t *endpoint,
                                          uint8_t expected_family) {
  if (endpoint->family != expected_family) {
    return false;
  }
  return expected_family != (uint8_t)AF_INET ||
         !utp_ntrs_bytes_nonzero(endpoint->address + 4u,
                                 sizeof(endpoint->address) - 4u);
}

static bool utp_ntrs_family_valid(const utp_ntrs_node_family_t *family,
                                  uint8_t expected_family) {
  if (!family->valid) {
    return true;
  }
  return family->family == expected_family &&
         utp_ntrs_endpoint_shape_valid(&family->public_endpoint,
                                       expected_family) &&
         utp_ntrs_endpoint_shape_valid(&family->probe_endpoint,
                                       expected_family) &&
         utp_ntrs_endpoint_shape_valid(&family->change_port_endpoint,
                                       expected_family) &&
         utp_ntrs_endpoint_shape_valid(&family->control_endpoint,
                                       expected_family) &&
         family->public_endpoint.port == 0u &&
         family->probe_endpoint.port != 0u &&
         family->change_port_endpoint.port != 0u &&
         family->control_endpoint.port != 0u &&
         family->probe_endpoint.port != family->change_port_endpoint.port &&
         utp_ntrs_endpoint_same_ip(&family->public_endpoint,
                                   &family->probe_endpoint) &&
         utp_ntrs_endpoint_same_ip(&family->public_endpoint,
                                   &family->change_port_endpoint);
}

static bool
utp_ntrs_registration_valid(const utp_ntrs_node_registration_t *registration) {
  return utp_ntrs_bytes_nonzero(registration->instance.node_id,
                                sizeof(registration->instance.node_id)) &&
         utp_ntrs_bytes_nonzero(registration->instance.boot_id,
                                sizeof(registration->instance.boot_id)) &&
         registration->heartbeat_ms != 0u &&
         (registration->ipv4.valid || registration->ipv6.valid) &&
         utp_ntrs_family_valid(&registration->ipv4, (uint8_t)AF_INET) &&
         utp_ntrs_family_valid(&registration->ipv6, (uint8_t)AF_INET6);
}

static utp_ntrs_hub_node_t *utp_ntrs_hub_nodes(utp_ntrs_hub_t *hub) {
  return (utp_ntrs_hub_node_t *)hub->nodes;
}

static const utp_ntrs_hub_node_t *
utp_ntrs_hub_nodes_const(const utp_ntrs_hub_t *hub) {
  return (const utp_ntrs_hub_node_t *)hub->nodes;
}

static int32_t utp_ntrs_hub_find(const utp_ntrs_hub_t *hub,
                                 const utp_ntrs_node_instance_t *instance,
                                 bool require_boot_id) {
  const utp_ntrs_hub_node_t *nodes = utp_ntrs_hub_nodes_const(hub);
  size_t index;

  for (index = 0u; index < hub->count; ++index) {
    if (utp_ntrs_node_id_equal(&nodes[index].registration.instance, instance) &&
        (!require_boot_id ||
         utp_ntrs_node_instance_equal(&nodes[index].registration.instance,
                                      instance))) {
      return (int32_t)index;
    }
  }
  return -1;
}

static uint8_t utp_ntrs_family_slot(uint8_t family) {
  return family == (uint8_t)AF_INET ? (uint8_t)0u : (uint8_t)1u;
}

static const utp_ntrs_node_family_t *
utp_ntrs_node_family_for(const utp_ntrs_hub_node_t *node, uint8_t family) {
  return family == (uint8_t)AF_INET ? &node->registration.ipv4
                                    : &node->registration.ipv6;
}

static bool utp_ntrs_is_excluded(const utp_ntrs_node_instance_t *instance,
                                 const utp_ntrs_node_instance_t *excluded,
                                 size_t excluded_count) {
  size_t index;

  for (index = 0u; index < excluded_count; ++index) {
    if (utp_ntrs_node_instance_equal(instance, &excluded[index])) {
      return true;
    }
  }
  return false;
}

static bool utp_ntrs_candidate_precedes(const utp_ntrs_hub_node_t *candidate,
                                        const utp_ntrs_hub_node_t *current) {
  int32_t result;

  if (current == NULL ||
      candidate->registration.load != current->registration.load) {
    return current == NULL ||
           candidate->registration.load < current->registration.load;
  }
  result = memcmp(candidate->registration.instance.node_id,
                  current->registration.instance.node_id,
                  sizeof(candidate->registration.instance.node_id));
  if (result != 0) {
    return result < 0;
  }
  return memcmp(candidate->registration.instance.boot_id,
                current->registration.instance.boot_id,
                sizeof(candidate->registration.instance.boot_id)) < 0;
}

static bool
utp_ntrs_assignment_targets_equal(const utp_ntrs_assignment_t *left,
                                  const utp_ntrs_assignment_t *right) {
  return left->has_primary == right->has_primary &&
         left->has_backup == right->has_backup &&
         (!left->has_primary ||
          (utp_ntrs_node_instance_equal(&left->primary, &right->primary) &&
           utp_ntrs_endpoint_equal(&left->primary_probe,
                                   &right->primary_probe) &&
           utp_ntrs_endpoint_equal(&left->primary_control,
                                   &right->primary_control))) &&
         (!left->has_backup ||
          (utp_ntrs_node_instance_equal(&left->backup, &right->backup) &&
           utp_ntrs_endpoint_equal(&left->backup_probe, &right->backup_probe) &&
           utp_ntrs_endpoint_equal(&left->backup_control,
                                   &right->backup_control)));
}

static bool utp_ntrs_node_excludes(const utp_ntrs_hub_node_t *node,
                                   uint8_t slot,
                                   const utp_ntrs_node_instance_t *candidate,
                                   uint64_t now_ms) {
  uint8_t index;

  for (index = 0u; index < node->exclusion_count[slot]; ++index) {
    if (node->exclusion_expire_at_ms[slot][index] > now_ms &&
        utp_ntrs_node_instance_equal(&node->exclusions[slot][index],
                                     candidate)) {
      return true;
    }
  }
  return false;
}

static void utp_ntrs_hub_prune_exclusions(utp_ntrs_hub_node_t *node,
                                          uint8_t slot, uint64_t now_ms) {
  uint8_t source;
  uint8_t destination = 0u;

  for (source = 0u; source < node->exclusion_count[slot]; ++source) {
    if (node->exclusion_expire_at_ms[slot][source] > now_ms) {
      if (destination != source) {
        node->exclusions[slot][destination] = node->exclusions[slot][source];
        node->exclusion_expire_at_ms[slot][destination] =
            node->exclusion_expire_at_ms[slot][source];
      }
      ++destination;
    }
  }
  node->exclusion_count[slot] = destination;
}

static void utp_ntrs_hub_exclude_node(utp_ntrs_hub_node_t *node, uint8_t slot,
                                      const utp_ntrs_node_instance_t *excluded,
                                      uint64_t now_ms) {
  uint8_t index;
  uint8_t replacement = 0u;

  for (index = 0u; index < node->exclusion_count[slot]; ++index) {
    if (utp_ntrs_node_instance_equal(&node->exclusions[slot][index],
                                     excluded)) {
      node->exclusion_expire_at_ms[slot][index] =
          now_ms + UTP_NTRS_ASSIGNMENT_EXCLUSION_MS;
      return;
    }
    if (node->exclusion_expire_at_ms[slot][index] <
        node->exclusion_expire_at_ms[slot][replacement]) {
      replacement = index;
    }
  }
  if (node->exclusion_count[slot] < UTP_NTRS_MAX_EXCLUDED_NODES) {
    replacement = node->exclusion_count[slot];
    ++node->exclusion_count[slot];
  }
  node->exclusions[slot][replacement] = *excluded;
  node->exclusion_expire_at_ms[slot][replacement] =
      now_ms + UTP_NTRS_ASSIGNMENT_EXCLUSION_MS;
}

static void utp_ntrs_build_assignment(const utp_ntrs_hub_t *hub,
                                      size_t target_index, uint8_t family,
                                      const utp_ntrs_node_instance_t *excluded,
                                      size_t excluded_count, uint64_t now_ms,
                                      utp_ntrs_assignment_t *assignment) {
  const utp_ntrs_hub_node_t *nodes = utp_ntrs_hub_nodes_const(hub);
  const utp_ntrs_node_family_t *target_family =
      utp_ntrs_node_family_for(&nodes[target_index], family);
  const utp_ntrs_hub_node_t *primary = NULL;
  const utp_ntrs_hub_node_t *backup = NULL;
  const uint8_t slot = utp_ntrs_family_slot(family);
  size_t index;

  *assignment = (utp_ntrs_assignment_t){.family = family};
  if (!target_family->valid) {
    return;
  }
  for (index = 0u; index < hub->count; ++index) {
    const utp_ntrs_hub_node_t *candidate = &nodes[index];
    const utp_ntrs_node_family_t *candidate_family =
        utp_ntrs_node_family_for(candidate, family);

    if (index == target_index || !candidate_family->valid ||
        utp_ntrs_endpoint_same_ip(&target_family->public_endpoint,
                                  &candidate_family->public_endpoint) ||
        utp_ntrs_is_excluded(&candidate->registration.instance, excluded,
                             excluded_count) ||
        utp_ntrs_node_excludes(&nodes[target_index], slot,
                               &candidate->registration.instance, now_ms)) {
      continue;
    }
    if (utp_ntrs_candidate_precedes(candidate, primary)) {
      primary = candidate;
    }
  }
  if (primary != NULL) {
    const utp_ntrs_node_family_t *primary_family =
        utp_ntrs_node_family_for(primary, family);

    assignment->primary = primary->registration.instance;
    assignment->primary_probe = primary_family->probe_endpoint;
    assignment->primary_control = primary_family->control_endpoint;
    assignment->has_primary = true;
    for (index = 0u; index < hub->count; ++index) {
      const utp_ntrs_hub_node_t *candidate = &nodes[index];
      const utp_ntrs_node_family_t *candidate_family =
          utp_ntrs_node_family_for(candidate, family);

      if (index == target_index || !candidate_family->valid ||
          utp_ntrs_endpoint_same_ip(&target_family->public_endpoint,
                                    &candidate_family->public_endpoint) ||
          utp_ntrs_endpoint_same_ip(&primary_family->public_endpoint,
                                    &candidate_family->public_endpoint) ||
          utp_ntrs_is_excluded(&candidate->registration.instance, excluded,
                               excluded_count) ||
          utp_ntrs_node_excludes(&nodes[target_index], slot,
                                 &candidate->registration.instance, now_ms)) {
        continue;
      }
      if (utp_ntrs_candidate_precedes(candidate, backup)) {
        backup = candidate;
      }
    }
  }
  if (backup != NULL) {
    const utp_ntrs_node_family_t *backup_family =
        utp_ntrs_node_family_for(backup, family);

    assignment->backup = backup->registration.instance;
    assignment->backup_probe = backup_family->probe_endpoint;
    assignment->backup_control = backup_family->control_endpoint;
    assignment->has_backup = true;
  }
}

static void utp_ntrs_hub_commit_assignment(utp_ntrs_assignment_t *current,
                                           const utp_ntrs_assignment_t *next) {
  if (current->version == 0u ||
      !utp_ntrs_assignment_targets_equal(current, next)) {
    const uint64_t version = current->version + 1u;

    *current = *next;
    current->version = version;
  }
}

static void utp_ntrs_hub_refresh_node_assignments(utp_ntrs_hub_t *hub,
                                                  size_t target_index,
                                                  uint64_t now_ms) {
  utp_ntrs_hub_node_t *const node = &utp_ntrs_hub_nodes(hub)[target_index];
  uint8_t slot;

  for (slot = 0u; slot < 2u; ++slot) {
    const uint8_t family = slot == 0u ? (uint8_t)AF_INET : (uint8_t)AF_INET6;
    utp_ntrs_assignment_t next;

    utp_ntrs_build_assignment(hub, target_index, family, NULL, 0u, now_ms,
                              &next);
    utp_ntrs_hub_commit_assignment(&node->assignments[slot], &next);
  }
}

static bool
utp_ntrs_hub_assignment_peer_online(const utp_ntrs_hub_t *hub,
                                    const utp_ntrs_node_instance_t *instance,
                                    uint8_t family) {
  const int32_t index = utp_ntrs_hub_find(hub, instance, true);

  return index >= 0 &&
         utp_ntrs_node_family_for(&utp_ntrs_hub_nodes_const(hub)[(size_t)index],
                                  family)
             ->valid;
}

static const utp_ntrs_hub_node_t *utp_ntrs_hub_find_candidate(
    const utp_ntrs_hub_t *hub, size_t target_index, uint8_t family,
    const utp_ntrs_assignment_t *assignment, uint64_t now_ms) {
  const utp_ntrs_hub_node_t *nodes = utp_ntrs_hub_nodes_const(hub);
  const utp_ntrs_node_family_t *target_family =
      utp_ntrs_node_family_for(&nodes[target_index], family);
  const utp_ntrs_hub_node_t *candidate = NULL;
  const uint8_t slot = utp_ntrs_family_slot(family);
  size_t index;

  for (index = 0u; index < hub->count; ++index) {
    const utp_ntrs_hub_node_t *current = &nodes[index];
    const utp_ntrs_node_family_t *current_family =
        utp_ntrs_node_family_for(current, family);

    if (index == target_index || !current_family->valid ||
        utp_ntrs_endpoint_same_ip(&target_family->public_endpoint,
                                  &current_family->public_endpoint) ||
        utp_ntrs_node_excludes(&nodes[target_index], slot,
                               &current->registration.instance, now_ms) ||
        (assignment->has_primary &&
         utp_ntrs_node_instance_equal(&assignment->primary,
                                      &current->registration.instance)) ||
        (assignment->has_backup &&
         utp_ntrs_node_instance_equal(&assignment->backup,
                                      &current->registration.instance))) {
      continue;
    }
    if (assignment->has_primary) {
      const int32_t primary_index =
          utp_ntrs_hub_find(hub, &assignment->primary, true);

      if (primary_index >= 0 &&
          utp_ntrs_endpoint_same_ip(
              &utp_ntrs_node_family_for(&nodes[(size_t)primary_index], family)
                   ->public_endpoint,
              &current_family->public_endpoint)) {
        continue;
      }
    }
    if (assignment->has_backup) {
      const int32_t backup_index =
          utp_ntrs_hub_find(hub, &assignment->backup, true);

      if (backup_index >= 0 &&
          utp_ntrs_endpoint_same_ip(
              &utp_ntrs_node_family_for(&nodes[(size_t)backup_index], family)
                   ->public_endpoint,
              &current_family->public_endpoint)) {
        continue;
      }
    }
    if (utp_ntrs_candidate_precedes(current, candidate)) {
      candidate = current;
    }
  }
  return candidate;
}

static void
utp_ntrs_hub_assignment_set_primary(utp_ntrs_assignment_t *assignment,
                                    const utp_ntrs_hub_node_t *node,
                                    uint8_t family) {
  const utp_ntrs_node_family_t *const node_family =
      utp_ntrs_node_family_for(node, family);

  assignment->primary = node->registration.instance;
  assignment->primary_probe = node_family->probe_endpoint;
  assignment->primary_control = node_family->control_endpoint;
  assignment->has_primary = true;
}

static void
utp_ntrs_hub_assignment_set_backup(utp_ntrs_assignment_t *assignment,
                                   const utp_ntrs_hub_node_t *node,
                                   uint8_t family) {
  const utp_ntrs_node_family_t *const node_family =
      utp_ntrs_node_family_for(node, family);

  assignment->backup = node->registration.instance;
  assignment->backup_probe = node_family->probe_endpoint;
  assignment->backup_control = node_family->control_endpoint;
  assignment->has_backup = true;
}

static void
utp_ntrs_hub_assignment_clear_primary(utp_ntrs_assignment_t *assignment) {
  assignment->primary = (utp_ntrs_node_instance_t){0};
  assignment->primary_probe = (utp_ntrs_endpoint_t){0};
  assignment->primary_control = (utp_ntrs_endpoint_t){0};
  assignment->has_primary = false;
}

static void
utp_ntrs_hub_assignment_clear_backup(utp_ntrs_assignment_t *assignment) {
  assignment->backup = (utp_ntrs_node_instance_t){0};
  assignment->backup_probe = (utp_ntrs_endpoint_t){0};
  assignment->backup_control = (utp_ntrs_endpoint_t){0};
  assignment->has_backup = false;
}

static void utp_ntrs_hub_fill_vacant_assignments(utp_ntrs_hub_t *hub,
                                                 uint64_t now_ms) {
  utp_ntrs_hub_node_t *const nodes = utp_ntrs_hub_nodes(hub);
  size_t index;
  uint8_t slot;

  for (index = 0u; index < hub->count; ++index) {
    for (slot = 0u; slot < 2u; ++slot) {
      const uint8_t family = slot == 0u ? (uint8_t)AF_INET : (uint8_t)AF_INET6;
      utp_ntrs_assignment_t next = nodes[index].assignments[slot];

      if (!utp_ntrs_node_family_for(&nodes[index], family)->valid) {
        continue;
      }
      if (!next.has_primary) {
        const utp_ntrs_hub_node_t *const candidate =
            utp_ntrs_hub_find_candidate(hub, index, family, &next, now_ms);

        if (candidate != NULL) {
          utp_ntrs_hub_assignment_set_primary(&next, candidate, family);
        }
      }
      if (!next.has_backup) {
        const utp_ntrs_hub_node_t *const candidate =
            utp_ntrs_hub_find_candidate(hub, index, family, &next, now_ms);

        if (candidate != NULL) {
          utp_ntrs_hub_assignment_set_backup(&next, candidate, family);
        }
      }
      utp_ntrs_hub_commit_assignment(&nodes[index].assignments[slot], &next);
    }
  }
}

void utp_ntrs_hub_init(utp_ntrs_hub_t *hub) { *hub = (utp_ntrs_hub_t){0}; }

void utp_ntrs_hub_destroy(utp_ntrs_hub_t *hub) {
  free(hub->nodes);
  *hub = (utp_ntrs_hub_t){0};
}

bool utp_ntrs_hub_register(utp_ntrs_hub_t *hub,
                           const utp_ntrs_node_registration_t *registration,
                           uint64_t now_ms, bool *replaced) {
  int32_t index;
  bool is_replaced = false;

  if (!utp_ntrs_registration_valid(registration)) {
    return false;
  }
  index = utp_ntrs_hub_find(hub, &registration->instance, false);
  if (index < 0) {
    utp_ntrs_hub_node_t *nodes;

    if (hub->count == hub->capacity) {
      const size_t new_capacity = hub->capacity == 0u ? 8u : hub->capacity * 2u;
      void *const new_nodes =
          realloc(hub->nodes, new_capacity * sizeof(utp_ntrs_hub_node_t));

      if (new_nodes == NULL) {
        return false;
      }
      hub->nodes = new_nodes;
      hub->capacity = new_capacity;
    }
    nodes = utp_ntrs_hub_nodes(hub);
    nodes[hub->count] = (utp_ntrs_hub_node_t){
        .registration = *registration,
        .last_heartbeat_ms = now_ms,
    };
    ++hub->count;
  } else {
    utp_ntrs_hub_node_t *node = &utp_ntrs_hub_nodes(hub)[(size_t)index];

    is_replaced = !utp_ntrs_node_instance_equal(&node->registration.instance,
                                                &registration->instance);
    if (is_replaced) {
      *node = (utp_ntrs_hub_node_t){
          .registration = *registration,
          .last_heartbeat_ms = now_ms,
      };
    } else {
      node->registration = *registration;
      node->last_heartbeat_ms = now_ms;
    }
  }
  if (index < 0 || is_replaced) {
    utp_ntrs_hub_refresh_node_assignments(
        hub, (size_t)(index < 0 ? (int32_t)(hub->count - 1u) : index), now_ms);
    utp_ntrs_hub_fill_vacant_assignments(hub, now_ms);
  }
  if (replaced != NULL) {
    *replaced = is_replaced;
  }
  return true;
}

bool utp_ntrs_hub_heartbeat(utp_ntrs_hub_t *hub,
                            const utp_ntrs_node_instance_t *instance,
                            uint32_t load, uint64_t now_ms) {
  const int32_t index = utp_ntrs_hub_find(hub, instance, true);

  if (index < 0) {
    return false;
  }
  utp_ntrs_hub_nodes(hub)[(size_t)index].registration.load = load;
  utp_ntrs_hub_nodes(hub)[(size_t)index].last_heartbeat_ms = now_ms;
  return true;
}

bool utp_ntrs_hub_remove(utp_ntrs_hub_t *hub,
                         const utp_ntrs_node_instance_t *instance,
                         uint64_t now_ms) {
  const int32_t index = utp_ntrs_hub_find(hub, instance, true);

  if (index < 0) {
    return false;
  }
  (void)now_ms;
  utp_ntrs_hub_nodes(hub)[(size_t)index] =
      utp_ntrs_hub_nodes(hub)[hub->count - 1u];
  --hub->count;
  return true;
}

size_t utp_ntrs_hub_sweep_expired(utp_ntrs_hub_t *hub, uint64_t now_ms) {
  utp_ntrs_hub_node_t *nodes = utp_ntrs_hub_nodes(hub);
  size_t index = 0u;
  size_t removed = 0u;

  while (index < hub->count) {
    const uint64_t timeout_ms =
        (uint64_t)nodes[index].registration.heartbeat_ms * 3u;

    if (now_ms - nodes[index].last_heartbeat_ms < timeout_ms) {
      ++index;
      continue;
    }
    nodes[index] = nodes[hub->count - 1u];
    --hub->count;
    ++removed;
  }
  return removed;
}

bool utp_ntrs_hub_get_assignment(const utp_ntrs_hub_t *hub,
                                 const utp_ntrs_node_instance_t *instance,
                                 uint8_t family,
                                 utp_ntrs_assignment_t *assignment) {
  const int32_t index = utp_ntrs_hub_find(hub, instance, true);

  if (index < 0 ||
      (family != (uint8_t)AF_INET && family != (uint8_t)AF_INET6)) {
    return false;
  }
  *assignment = utp_ntrs_hub_nodes_const(hub)[(size_t)index]
                    .assignments[utp_ntrs_family_slot(family)];
  return true;
}

bool utp_ntrs_hub_request_assignment(
    utp_ntrs_hub_t *hub, const utp_ntrs_assignment_request_t *request,
    uint64_t now_ms, utp_ntrs_assignment_t *assignment) {
  const int32_t index = utp_ntrs_hub_find(hub, &request->instance, true);
  utp_ntrs_hub_node_t *node;
  utp_ntrs_assignment_t *current;
  utp_ntrs_assignment_t next;
  uint8_t slot;

  if (index < 0 ||
      (request->family != (uint8_t)AF_INET &&
       request->family != (uint8_t)AF_INET6) ||
      request->failed_roles == 0u ||
      (request->failed_roles & ~(UTP_NTRS_ASSIGNMENT_ROLE_PRIMARY |
                                 UTP_NTRS_ASSIGNMENT_ROLE_BACKUP)) != 0u) {
    return false;
  }
  node = &utp_ntrs_hub_nodes(hub)[(size_t)index];
  slot = utp_ntrs_family_slot(request->family);
  current = &node->assignments[slot];
  if (current->version != request->assignment_version ||
      ((request->failed_roles & UTP_NTRS_ASSIGNMENT_ROLE_PRIMARY) != 0u &&
       (!current->has_primary ||
        !utp_ntrs_node_instance_equal(&current->primary,
                                      &request->failed_primary))) ||
      ((request->failed_roles & UTP_NTRS_ASSIGNMENT_ROLE_BACKUP) != 0u &&
       (!current->has_backup ||
        !utp_ntrs_node_instance_equal(&current->backup,
                                      &request->failed_backup)))) {
    *assignment = *current;
    return true;
  }
  utp_ntrs_hub_prune_exclusions(node, slot, now_ms);
  if ((request->failed_roles & UTP_NTRS_ASSIGNMENT_ROLE_PRIMARY) != 0u) {
    utp_ntrs_hub_exclude_node(node, slot, &request->failed_primary, now_ms);
  }
  if ((request->failed_roles & UTP_NTRS_ASSIGNMENT_ROLE_BACKUP) != 0u) {
    utp_ntrs_hub_exclude_node(node, slot, &request->failed_backup, now_ms);
  }
  next = *current;
  if ((request->failed_roles & UTP_NTRS_ASSIGNMENT_ROLE_PRIMARY) != 0u) {
    if (next.has_backup &&
        (request->failed_roles & UTP_NTRS_ASSIGNMENT_ROLE_BACKUP) == 0u &&
        utp_ntrs_hub_assignment_peer_online(hub, &next.backup,
                                            request->family)) {
      utp_ntrs_hub_assignment_set_primary(
          &next,
          &utp_ntrs_hub_nodes_const(
              hub)[(size_t)utp_ntrs_hub_find(hub, &next.backup, true)],
          request->family);
      utp_ntrs_hub_assignment_clear_backup(&next);
    } else {
      utp_ntrs_hub_assignment_clear_primary(&next);
    }
  }
  if ((request->failed_roles & UTP_NTRS_ASSIGNMENT_ROLE_BACKUP) != 0u) {
    utp_ntrs_hub_assignment_clear_backup(&next);
  }
  if (!next.has_primary) {
    const utp_ntrs_hub_node_t *const candidate = utp_ntrs_hub_find_candidate(
        hub, (size_t)index, request->family, &next, now_ms);

    if (candidate != NULL) {
      utp_ntrs_hub_assignment_set_primary(&next, candidate, request->family);
    }
  }
  if (!next.has_backup) {
    const utp_ntrs_hub_node_t *const candidate = utp_ntrs_hub_find_candidate(
        hub, (size_t)index, request->family, &next, now_ms);

    if (candidate != NULL) {
      utp_ntrs_hub_assignment_set_backup(&next, candidate, request->family);
    }
  }
  utp_ntrs_hub_commit_assignment(current, &next);
  *assignment = *current;
  return true;
}
