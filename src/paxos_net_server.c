// SPDX-FileCopyrightText: 2026 Andy Curtis <contactandyc@gmail.com>
// SPDX-License-Identifier: Apache-2.0
//
// Maintainer: Andy Curtis <contactandyc@gmail.com>

#include "a-paxos-net-library/paxos_net.h"
#include <uv.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>

#define MAX_PENDING_CLIENT_REQS 16384

typedef struct incoming_cmd_s {
    h2o_c_req_t *http_req;
    uint64_t client_seq;
    uint8_t op;
    char *key; uint32_t klen;
    char *val; uint32_t vlen;
    struct incoming_cmd_s *next;
} incoming_cmd_t;

struct paxos_server_s {
    paxos_t *paxos;
    paxos_app_machine_t app;
    paxos_server_options_t opts;

    // Cross-Thread Queue
    pthread_mutex_t cmd_mutex;
    incoming_cmd_t *cmd_queue_head;
    uv_async_t cmd_wakeup;
    _Atomic uint64_t seq_generator;

    // Suspended HTTP Map
    h2o_c_req_t *pending_http_reqs[MAX_PENDING_CLIENT_REQS];

    uv_thread_t paxos_thread;
    uv_loop_t paxos_loop;
    uv_timer_t paxos_timer;
    bool running;
};

// --- HTTP REQUEST HANDLER (Runs on H2O Worker Threads) ---
static void http_handler(h2o_c_req_t *req, void *arg, const char *method, const char *path, h2o_c_header_t *in_headers, const char *body, size_t body_len) {
    paxos_server_t *s = (paxos_server_t *)arg;

    if (strcmp(method, "GET") == 0) {
        s->app.on_http_get(s->app.app_ctx, req, path);
        return;
    }

    if (strcmp(method, "PUT") == 0 || strcmp(method, "DELETE") == 0) {
        incoming_cmd_t *cmd = calloc(1, sizeof(incoming_cmd_t));
        cmd->http_req = req;
        cmd->client_seq = atomic_fetch_add(&s->seq_generator, 1);
        cmd->op = (strcmp(method, "PUT") == 0) ? 0 : 1;

        cmd->key = strdup(path + 5); // Assumes /key/my_key
        cmd->klen = strlen(cmd->key);
        if (body_len > 0) {
            cmd->val = malloc(body_len);
            memcpy(cmd->val, body, body_len);
            cmd->vlen = body_len;
        }

        pthread_mutex_lock(&s->cmd_mutex);
        cmd->next = s->cmd_queue_head;
        s->cmd_queue_head = cmd;
        pthread_mutex_unlock(&s->cmd_mutex);

        uv_async_send(&s->cmd_wakeup);
        return;
    }

    h2o_c_response_t *err = h2o_c_make_response(400, "Bad Request", "Invalid Method", 14, "text/plain");
    h2o_c_send_response(req, err);
}

// --- COMMAND INJECTOR (Runs on Paxos Thread) ---
static void on_cmd_wakeup(uv_async_t *handle) {
    paxos_server_t *s = (paxos_server_t *)handle->data;

    pthread_mutex_lock(&s->cmd_mutex);
    incoming_cmd_t *curr = s->cmd_queue_head;
    s->cmd_queue_head = NULL;
    pthread_mutex_unlock(&s->cmd_mutex);

    while (curr) {
        uint32_t slot_idx = curr->client_seq % MAX_PENDING_CLIENT_REQS;
        if (s->pending_http_reqs[slot_idx] != NULL) {
            h2o_c_response_t *timeout = h2o_c_make_response(503, "Service Unavailable", "Timeout", 7, "text/plain");
            h2o_c_send_response(s->pending_http_reqs[slot_idx], timeout);
        }
        s->pending_http_reqs[slot_idx] = curr->http_req;

        size_t payload_len = 9 + curr->klen + curr->vlen;
        uint8_t *payload = malloc(payload_len);
        payload[0] = curr->op;
        *(uint32_t*)(payload + 1) = curr->klen;
        *(uint32_t*)(payload + 5) = curr->vlen;
        memcpy(payload + 9, curr->key, curr->klen);
        if (curr->vlen > 0) memcpy(payload + 9 + curr->klen, curr->val, curr->vlen);

        paxos_propose(s->paxos, s->opts.node_id, curr->client_seq, payload, payload_len);

        free(payload);
        free(curr->key);
        if (curr->val) free(curr->val);
        incoming_cmd_t *next = curr->next;
        free(curr);
        curr = next;
    }
}

// --- THE STRICT COMMIT PROTOCOL ---
static void process_paxos_ready(paxos_server_t *s) {
    paxos_ready_t ready;
    if (paxos_get_ready(s->paxos, &ready) != PAXOS_OK) return;

    // 1. Immediate Network (NACKs, ReadBarriers)
    for (size_t i = 0; i < ready.num_messages_immediate; i++) {
        p2p_network_send(s, ready.messages_immediate[i].to, &ready.messages_immediate[i]);
    }

    // 2. Strict Persistence (The Consensus WAL)
    if (ready.num_entries_to_save > 0) {
        // paxos_disk_log_append_batch(s->disk_log, ready.entries_to_save, ready.num_entries_to_save);
        // paxos_disk_log_sync(s->disk_log); // Blocks until hardware safe
    }

    // 3. Post-Persist Network (Promises, Accepts)
    for (size_t i = 0; i < ready.num_messages_after_persist; i++) {
        p2p_network_send(s, ready.messages_after_persist[i].to, &ready.messages_after_persist[i]);
    }

    // 4. Apply Application State
    uint64_t max_applied = 0;
    for (size_t i = 0; i < ready.num_chosen_entries; i++) {
        paxos_entry_t *e = &ready.chosen_entries[i];

        if (e->type == PAXOS_ENTRY_NORMAL) {
            uint8_t op = e->data[0];
            uint32_t klen = *(uint32_t*)(e->data + 1);
            uint32_t vlen = *(uint32_t*)(e->data + 5);
            const void *key = e->data + 9;
            const void *val = e->data + 9 + klen;

            s->app.on_apply(s->app.app_ctx, e->slot, op, key, klen, val, vlen);

            // Wake up the suspended HTTP request
            if (e->client_id == s->opts.node_id) {
                uint32_t slot_idx = e->client_seq % MAX_PENDING_CLIENT_REQS;
                h2o_c_req_t *req = s->pending_http_reqs[slot_idx];
                if (req) {
                    h2o_c_response_t *resp = h2o_c_make_response(200, "OK", "Success", 7, "text/plain");
                    h2o_c_send_response(req, resp);
                    s->pending_http_reqs[slot_idx] = NULL;
                }
            }
        }
        max_applied = e->slot;
    }

    // 5. Catch-up Interception
    if (ready.needs_catchup) {
        // ... wake up sync manager ...
    }

    if (max_applied > 0 || ready.num_entries_to_save > 0) {
        paxos_advance(s->paxos, NULL, 0, max_applied);
    }
}

// --- LIFECYCLE ---
static void on_paxos_tick(uv_timer_t *handle) {
    paxos_server_t *s = (paxos_server_t *)handle->data;
    paxos_tick(s->paxos);
    process_paxos_ready(s);
}

static void paxos_thread_entry(void *arg) {
    paxos_server_t *s = (paxos_server_t *)arg;
    uv_loop_init(&s->paxos_loop);

    uv_async_init(&s->paxos_loop, &s->cmd_wakeup, on_cmd_wakeup);
    s->cmd_wakeup.data = s;

    uv_timer_init(&s->paxos_loop, &s->paxos_timer);
    s->paxos_timer.data = s;
    uv_timer_start(&s->paxos_timer, on_paxos_tick, s->opts.tick_ms, s->opts.tick_ms);

    // p2p_network_listen(&s->paxos_loop, s->opts.p2p_port);

    uv_run(&s->paxos_loop, UV_RUN_DEFAULT);
    uv_loop_close(&s->paxos_loop);
}

paxos_server_t *paxos_server_init(paxos_config_t *paxos_cfg, paxos_app_machine_t *app, paxos_server_options_t *opts) {
    paxos_server_t *s = calloc(1, sizeof(paxos_server_t));
    s->opts = *opts;
    s->app = *app;
    pthread_mutex_init(&s->cmd_mutex, NULL);
    paxos_create(paxos_cfg, &s->paxos);
    return s;
}

void paxos_server_run(paxos_server_t *s) {
    s->running = true;
    uv_thread_create(&s->paxos_thread, paxos_thread_entry, s);

    h2o_c_init(&s->opts.h2o);
    h2o_c_use("GET", NULL, http_handler, s);
    h2o_c_use("PUT", NULL, http_handler, s);
    h2o_c_use("DELETE", NULL, http_handler, s);
    h2o_c_run();
}
