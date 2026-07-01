// SPDX-FileCopyrightText: 2026 Andy Curtis <contactandyc@gmail.com>
// SPDX-License-Identifier: Apache-2.0
//
// Maintainer: Andy Curtis <contactandyc@gmail.com>

#ifndef PAXOS_NET_INTERNAL_H
#define PAXOS_NET_INTERNAL_H

#include "a-paxos-net-library/paxos_net.h"
#include <uv.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

#define MAX_PENDING_CLIENT_REQS 16384

typedef struct incoming_cmd_s {
    h2o_c_req_t *http_req;
    uint64_t client_seq;
    uint8_t op;
    char *key; uint32_t klen;
    char *val; uint32_t vlen;
    struct incoming_cmd_s *next;
} incoming_cmd_t;

// Define the internal Sync Manager state
typedef enum {
    SYNC_IDLE = 0,
    SYNC_EVALUATING,
    SYNC_DOWNLOADING_FILE,
    SYNC_FETCH_LOG
} sync_state_t;

typedef struct {
    sync_state_t state;
    uint64_t target_index;
    uint64_t active_target_node;
    uint64_t last_progress_ms;
} sync_manager_t;

// The main server struct is now visible to all internal .c files
struct paxos_server_s {
    paxos_t *paxos;
    paxos_app_machine_t app;
    paxos_server_options_t opts;

    h2o_c_server_t *http_server;

    void *p2p_state; // Container for the P2P networking state

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

    sync_manager_t sync_mgr;
};

// Internal Cross-File Prototypes
void sync_manager_tick(paxos_server_t *s);
void p2p_network_destroy(paxos_server_t *s);

#endif // PAXOS_NET_INTERNAL_H
