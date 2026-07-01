// SPDX-FileCopyrightText: 2026 Andy Curtis <contactandyc@gmail.com>
// SPDX-License-Identifier: Apache-2.0
//
// Maintainer: Andy Curtis <contactandyc@gmail.com>

#ifndef A_PAXOS_NET_H
#define A_PAXOS_NET_H

#include <a-paxos-core/paxos.h>
#include <h2o-c-library/h2o_c.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct paxos_server_s paxos_server_t;

/* * The Application Interface:
 * The network layer calls these to interact with the Storage Engine (LSM).
 */
typedef struct {
    void *app_ctx; // Your lsm_env_t or lsm_db_t

    // Fired when Paxos safely commits a write.
    void (*on_apply)(void *app_ctx, uint64_t slot, uint8_t op, const void *key, uint32_t klen, const void *val, uint32_t vlen);

    // Fired when an HTTP GET request arrives (Read-Only)
    void (*on_http_get)(void *app_ctx, h2o_c_req_t *req, const char *path);

    // Fired when the sync manager needs to stream SSTables to a Learner
    void (*on_snapshot_create)(void *app_ctx, uint64_t *out_snapshot_index);
    void (*on_snapshot_chunk_recv)(void *app_ctx, uint64_t slot, uint64_t offset, const uint8_t *data, size_t len, bool done);
} paxos_app_machine_t;

typedef struct {
    uint64_t node_id;
    int p2p_port;           // Port for raw binary Paxos RPC (e.g., 9090)
    int tick_ms;            // Paxos tick rate (e.g., 10ms)
    h2o_c_options_t h2o;    // Port for HTTP Client REST API (e.g., 8080)
} paxos_server_options_t;

/* Lifecycle API */
paxos_server_t *paxos_server_init(paxos_config_t *paxos_cfg, paxos_app_machine_t *app, paxos_server_options_t *opts);
void paxos_server_run(paxos_server_t *server); // Blocks the calling thread
void paxos_server_stop(paxos_server_t *server);
void paxos_server_destroy(paxos_server_t *server);

void p2p_network_init(paxos_server_t *s);

/* Internal P2P API (Used by Sync Manager) */
int p2p_network_get_latency(paxos_server_t *s, uint64_t node_id);
void p2p_network_send(paxos_server_t *s, uint64_t to_node, paxos_msg_t *msg);

#endif /* A_PAXOS_NET_H */
