// SPDX-FileCopyrightText: 2026 Andy Curtis <contactandyc@gmail.com>
// SPDX-License-Identifier: Apache-2.0
//
// Maintainer: Andy Curtis <contactandyc@gmail.com>

#include "a-paxos-net-library/paxos_net.h"

// Internal State Machine definition
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

static uint64_t current_time_ms() {
    return uv_hrtime() / 1000000;
}

static uint64_t select_best_sync_peer(paxos_server_t *s, uint64_t my_commit_index, uint64_t target_index) {
    uint64_t *peers = NULL;
    size_t num_peers = paxos_get_peers(s->paxos, &peers);
    if (num_peers == 0) return 0;

    uint64_t best_peer = 0;
    int best_score = -1;

    for (size_t i = 0; i < num_peers; i++) {
        uint64_t node_id = peers[i];
        if (node_id == s->opts.node_id) continue;

        uint64_t peer_idx = paxos_peer_commit_index(s->paxos, node_id);
        if (peer_idx < target_index && peer_idx <= my_commit_index) continue;

        bool is_leader = paxos_peer_is_leader(s->paxos, node_id);
        int latency_ms = p2p_network_get_latency(s, node_id);
        if (latency_ms < 0) continue;

        int score = latency_ms;
        if (is_leader) score += 10000; // Penalize leader

        if (best_score == -1 || score < best_score) {
            best_score = score;
            best_peer = node_id;
        }
    }
    free(peers);
    return best_peer;
}

// Called periodically by the libuv event loop in the Paxos Thread
void sync_manager_tick(paxos_server_t *s, sync_manager_t *sm) {
    if (sm->state == SYNC_IDLE) return;

    uint64_t now = current_time_ms();

    // Timeout detection
    if (sm->state != SYNC_EVALUATING && (now - sm->last_progress_ms > 2000)) {
        sm->active_target_node = 0;
        sm->state = SYNC_EVALUATING;
    }

    if (sm->state == SYNC_EVALUATING) {
        sm->active_target_node = select_best_sync_peer(s, paxos_local_commit_index(s->paxos), sm->target_index);
        if (sm->active_target_node != 0) {
            sm->last_progress_ms = now;
            sm->state = SYNC_FETCH_LOG;

            paxos_msg_t fetch = {
                .type = PAXOS_MSG_FETCH_ENTRIES,
                .to = sm->active_target_node,
                .ballot = paxos_promised_ballot(s->paxos),
                .slot = paxos_local_commit_index(s->paxos) + 1
            };
            p2p_network_send(s, fetch.to, &fetch);
        }
    }
}
