// SPDX-FileCopyrightText: 2026 Andy Curtis <contactandyc@gmail.com>
// SPDX-License-Identifier: Apache-2.0
//
// Maintainer: Andy Curtis <contactandyc@gmail.com>

#include "a-table-store-library/lsm_env.h"
#include "a-table-store-library/lsm_db.h"
#include "a-table-store-library/lsm_posix.h" // Needed for local_posix_backend
#include "a-paxos-net-library/paxos_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Application Callbacks ---

// Fired by the Paxos thread when a write successfully achieves consensus
static void on_paxos_apply(void *app_ctx, uint64_t slot, uint8_t op, const void *key, uint32_t klen, const void *val, uint32_t vlen) {
    lsm_db_t *db = (lsm_db_t *)app_ctx;

    // The Paxos Slot acts as the strict MVCC Sequence Number for the LSM!
    if (op == 0 /* OP_PUT */) {
        lsm_db_put(db, key, klen, val, vlen);
    } else if (op == 1 /* OP_DELETE */) {
        lsm_db_delete(db, key, klen);
    }
}

// Fired by H2O threads for Read-Only GET requests
static void on_http_get(void *app_ctx, h2o_c_req_t *req, const char *path) {
    lsm_db_t *db = (lsm_db_t *)app_ctx;

    // Route: GET /key/{actual_key}
    if (strncmp(path, "/key/", 5) != 0) {
        h2o_c_response_t *resp = h2o_c_make_response(400, "Bad Request", "Invalid Path", 12, "text/plain");
        h2o_c_send_response(req, resp);
        return;
    }

    const char *key = path + 5;
    uint32_t klen = strlen(key);
    uint32_t vlen = 0;

    // Read the absolute latest committed state (UINT64_MAX)
    void *val = lsm_db_get(db, key, klen, UINT64_MAX, &vlen);

    if (!val) {
        h2o_c_response_t *resp = h2o_c_make_response(404, "Not Found", "Key not found", 13, "text/plain");
        h2o_c_send_response(req, resp);
        return;
    }

    // h2o_c_make_response will copy the buffer, so we can free the LSM allocation safely
    h2o_c_response_t *resp = h2o_c_make_response(200, "OK", val, vlen, "application/octet-stream");
    h2o_c_send_response(req, resp);
    free(val);
}

// Dummy snapshot callbacks to prevent segfaults
static void on_snapshot_create(void *ctx, uint64_t *out_idx) {
    (void)ctx;
    *out_idx = 0;
}

static void on_snapshot_chunk_recv(void *ctx, uint64_t s, uint64_t o, const uint8_t *d, size_t l, bool f) {
    (void)ctx; (void)s; (void)o; (void)d; (void)l; (void)f;
}

// --- Main Daemon ---

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <node_id>\n", argv[0]);
        return 1;
    }

    uint64_t my_node_id = strtoull(argv[1], NULL, 10);
    printf("Starting Distributed DB Node %llu...\n", (unsigned long long)my_node_id);

    // 1. Boot the Storage Engine
    size_t cache_size = 1024 * 1024 * 100; // 100MB Block Cache
    size_t mem_limit = 1024 * 1024 * 500;  // 500MB Global MemTable Limit

    // Note: global_wal is passed as NULL. Paxos acts as our durable WAL.
    lsm_env_t *env = lsm_env_init(cache_size, mem_limit, 4, &local_posix_backend, &local_posix_backend, 2, NULL);

    char db_dir[256];
    snprintf(db_dir, sizeof(db_dir), "/tmp/paxos_node_%llu_data", (unsigned long long)my_node_id);
    lsm_db_t *db = lsm_db_open(env, 1, db_dir);

    // 2. Wire the App to Paxos
    paxos_app_machine_t app = {
        .app_ctx = db,
        .on_apply = on_paxos_apply,
        .on_http_get = on_http_get,
        .on_snapshot_create = on_snapshot_create,
        .on_snapshot_chunk_recv = on_snapshot_chunk_recv
    };

    // 3. Configure Paxos Topology
    // Assuming a 3-node cluster: IDs 1, 2, and 3.
    uint64_t initial_voters[] = {1, 2, 3};
    paxos_config_t pcfg = {
        .struct_size = sizeof(paxos_config_t),
        .node_id = my_node_id,
        .initial_voters = initial_voters,
        .num_initial_voters = 3,
        .heartbeat_ticks = 10,
        .election_ticks = 30,
        .max_payload_size = 4 * 1024 * 1024 // 4MB
    };

    // 4. Configure Networking
    paxos_server_options_t net_opts = {
        .node_id = my_node_id,
        .p2p_port = 9000 + (int)my_node_id, // e.g., 9001
        .tick_ms = 10,                 // 10ms Paxos tick resolution
        .h2o = {
            .enable_ssl = false,
            .enable_http2 = true,
            .thread_pool_size = 4,
            .port = 8000 + (int)my_node_id, // e.g., 8001
            .address = "0.0.0.0"
        }
    };

    // 5. Ignite the Server
    paxos_server_t *server = paxos_server_init(&pcfg, &app, &net_opts);

    printf("Server running. HTTP API on port %d, P2P on port %d.\n", net_opts.h2o.port, net_opts.p2p_port);
    paxos_server_run(server); // Blocks forever handling traffic

    // (Cleanup omitted for brevity)
    return 0;
}
