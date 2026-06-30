// SPDX-FileCopyrightText: 2026 Andy Curtis <contactandyc@gmail.com>
// SPDX-License-Identifier: Apache-2.0
//
// Maintainer: Andy Curtis <contactandyc@gmail.com>

#include "a-paxos-net-library/paxos_net.h"
#include <uv.h>
#include <stdlib.h>
#include <string.h>

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

// (Network setup, uv_read_cb, and uv_write_cb implementations omitted for brevity,
// but they handle the 4-byte length framing to reconstruct full packets)

static void network_send_raw(peer_connection_t *conn, uint8_t msg_type, void *payload, size_t len) {
    if (!conn->is_connected) return;
    // ... allocates buffer [Len (4)] [Type (1)] [Payload (len)] and calls uv_write
}

// --- LATENCY TRACKING ---

static void on_ping_timer(uv_timer_t *handle) {
    peer_connection_t *conn = (peer_connection_t *)handle->data;
    uint64_t now_us = uv_hrtime() / 1000;
    network_send_raw(conn, NET_MSG_PING, &now_us, sizeof(uint64_t));
}

void p2p_handle_incoming_frame(peer_connection_t *conn, uint8_t msg_type, uint8_t *payload, size_t len) {
    if (msg_type == NET_MSG_PING) {
        // Instantly echo the timestamp back
        network_send_raw(conn, NET_MSG_PONG, payload, len);
    }
    else if (msg_type == NET_MSG_PONG) {
        // Calculate EWMA Latency
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
        // Deserialize and pass to the core
        paxos_msg_t msg;
        // p2p_deserialize_msg(payload, len, &msg);
        paxos_receive(conn->server->paxos, &msg);
    }
}

int p2p_network_get_latency(paxos_server_t *s, uint64_t node_id) {
    // Lookup peer_connection_t from array/hashmap...
    peer_connection_t *conn = NULL; // = lookup_peer(s, node_id);
    if (!conn || !conn->is_connected) return -1;
    return (int)conn->smoothed_latency_ms;
}

void p2p_network_send(paxos_server_t *s, uint64_t to_node, paxos_msg_t *msg) {
    peer_connection_t *conn = NULL; // = lookup_peer(s, to_node);
    if (!conn || !conn->is_connected) return;

    // size_t len;
    // uint8_t *buf = p2p_serialize_msg(msg, &len);
    // network_send_raw(conn, NET_MSG_PAXOS, buf, len);
    // free(buf);
}
