// SPDX-FileCopyrightText: 2026 Andy Curtis <contactandyc@gmail.com>
// SPDX-License-Identifier: Apache-2.0
//
// Maintainer: Andy Curtis <contactandyc@gmail.com>

#include "paxos_net_internal.h"
#include <string.h>
#include <stdio.h>


#define NET_MSG_PAXOS 1
#define NET_MSG_PING  2
#define NET_MSG_PONG  3

typedef struct {
    uint64_t peer_node_id;
    uv_tcp_t socket;
    uv_timer_t ping_timer;
    double smoothed_latency_ms;
    bool is_connected;
    paxos_server_t *server;
} peer_connection_t;

static void network_send_raw(peer_connection_t *conn, uint8_t msg_type, void *payload, size_t len) {
    if (!conn || !conn->is_connected) return;
    (void)msg_type; (void)payload; (void)len;
}

void p2p_handle_incoming_frame(peer_connection_t *conn, uint8_t msg_type, uint8_t *payload, size_t len) {
    if (msg_type == NET_MSG_PING) {
        network_send_raw(conn, NET_MSG_PONG, payload, len);
    }
    else if (msg_type == NET_MSG_PONG) {
        uint64_t sent_us;
        memcpy(&sent_us, payload, sizeof(uint64_t));
        uint64_t now_us = uv_hrtime() / 1000;

        double rtt_ms = (double)(now_us - sent_us) / 1000.0;

        if (conn->smoothed_latency_ms == 0.0) {
            conn->smoothed_latency_ms = rtt_ms;
        } else {
            conn->smoothed_latency_ms = (0.2 * rtt_ms) + (0.8 * conn->smoothed_latency_ms);
        }
    }
    else if (msg_type == NET_MSG_PAXOS) {
        paxos_msg_t msg;
        // p2p_deserialize_msg(payload, len, &msg);

        // PRODUCTION FIX: Catch and handle receive errors
        paxos_err_t err = paxos_receive(conn->server->paxos, &msg);
        if (err != PAXOS_OK) {
            fprintf(stderr, "[P2P] Warning: Paxos engine rejected message from node %llu (Error: %d)\n",
                    (unsigned long long)msg.from, err);

            // If the engine suffered a catastrophic failure, safely halt the event loop
            if (paxos_has_fatal_error(conn->server->paxos)) {
                fprintf(stderr, "FATAL: Paxos engine corrupted. Halting node.\n");
                uv_stop(&conn->server->paxos_loop);
            }
        }
    }
}

int p2p_network_get_latency(paxos_server_t *s, uint64_t node_id) {
    (void)s; (void)node_id;
    peer_connection_t *conn = NULL;
    if (!conn || !conn->is_connected) return -1;
    return (int)conn->smoothed_latency_ms;
}

void p2p_network_send(paxos_server_t *s, uint64_t to_node, paxos_msg_t *msg) {
    (void)s; (void)to_node; (void)msg;
    peer_connection_t *conn = NULL;
    if (!conn || !conn->is_connected) return;
}
