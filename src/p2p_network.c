// SPDX-FileCopyrightText: 2026 Andy Curtis <contactandyc@gmail.com>
// SPDX-License-Identifier: Apache-2.0

#include "paxos_net_internal.h"
#include <uv.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define NET_MSG_PAXOS 1
#define NET_MSG_PING  2
#define NET_MSG_PONG  3

typedef struct {
    uint64_t peer_node_id;
    uv_tcp_t socket;
    uv_connect_t connect_req;
    uv_timer_t reconnect_timer;
    uv_timer_t ping_timer;
    double smoothed_latency_ms;
    bool is_connected;
    paxos_server_t *server;

    uint8_t *read_buf;
    size_t read_pos;
    size_t read_cap;
} peer_connection_t;

// Hardcoded for the 3-Node Example
#define MAX_PEERS 4
static peer_connection_t g_peers[MAX_PEERS];
static uv_tcp_t g_listener;

// --- SERIALIZATION ---

static inline void enc32(uint8_t *dst, uint32_t v) {
    dst[0] = v & 0xFF; dst[1] = (v >> 8) & 0xFF; dst[2] = (v >> 16) & 0xFF; dst[3] = (v >> 24) & 0xFF;
}
static inline uint32_t dec32(const uint8_t *src) {
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
}
static inline void enc64(uint8_t *dst, uint64_t v) {
    for (int i = 0; i < 8; i++) dst[i] = (v >> (i * 8)) & 0xFF;
}
static inline uint64_t dec64(const uint8_t *src) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= ((uint64_t)src[i]) << (i * 8);
    return v;
}

static uint8_t *p2p_serialize_msg(const paxos_msg_t *msg, size_t *out_len) {
    // PRODUCTION FIX: Defensive clamping. Protect against uninitialized stack
    // memory from the core engine by enforcing length limits if pointers are NULL.
    uint32_t safe_num_entries = (msg->entries != NULL) ? msg->num_entries : 0;
    uint32_t safe_snap_len = (msg->snapshot_data != NULL) ? msg->snapshot_len : 0;

    size_t req_len = 66; // Base size
    for (size_t i = 0; i < safe_num_entries; i++) {
        req_len += 21 + msg->entries[i].data_len;
    }
    req_len += safe_snap_len;

    uint8_t *buf = malloc(req_len);
    size_t ptr = 0;

    enc64(buf + ptr, msg->to); ptr += 8;
    enc64(buf + ptr, msg->from); ptr += 8;
    enc64(buf + ptr, msg->ballot); ptr += 8;
    enc64(buf + ptr, msg->promised_ballot); ptr += 8;
    enc64(buf + ptr, msg->slot); ptr += 8;
    enc64(buf + ptr, msg->commit_index); ptr += 8;
    enc64(buf + ptr, msg->read_seq); ptr += 8;
    enc64(buf + ptr, msg->value_hash); ptr += 8;

    buf[ptr++] = msg->type;
    buf[ptr++] = msg->reject ? 1 : 0;
    buf[ptr++] = msg->snapshot_done ? 1 : 0;

    enc32(buf + ptr, safe_num_entries); ptr += 4;
    for (size_t i = 0; i < safe_num_entries; i++) {
        paxos_entry_t *e = &msg->entries[i];
        enc64(buf + ptr, e->slot); ptr += 8;
        enc64(buf + ptr, e->accepted_ballot); ptr += 8;
        buf[ptr++] = e->type;
        enc32(buf + ptr, (uint32_t)e->data_len); ptr += 4;
        if (e->data_len > 0 && e->data != NULL) {
            memcpy(buf + ptr, e->data, e->data_len);
            ptr += e->data_len;
        }
    }

    enc64(buf + ptr, msg->snapshot_offset); ptr += 8;
    enc32(buf + ptr, safe_snap_len); ptr += 4;
    if (safe_snap_len > 0) {
        memcpy(buf + ptr, msg->snapshot_data, safe_snap_len);
        ptr += safe_snap_len;
    }

    *out_len = ptr;
    return buf;
}

static void p2p_deserialize_msg(const uint8_t *buf, size_t len, paxos_msg_t *msg) {
    memset(msg, 0, sizeof(paxos_msg_t));
    if (len < 66) return;

    size_t ptr = 0;
    msg->to = dec64(buf + ptr); ptr += 8;
    msg->from = dec64(buf + ptr); ptr += 8;
    msg->ballot = dec64(buf + ptr); ptr += 8;
    msg->promised_ballot = dec64(buf + ptr); ptr += 8;
    msg->slot = dec64(buf + ptr); ptr += 8;
    msg->commit_index = dec64(buf + ptr); ptr += 8;
    msg->read_seq = dec64(buf + ptr); ptr += 8;
    msg->value_hash = dec64(buf + ptr); ptr += 8;

    msg->type = buf[ptr++];
    msg->reject = buf[ptr++] == 1;
    msg->snapshot_done = buf[ptr++] == 1;

    msg->num_entries = dec32(buf + ptr); ptr += 4;
    if (msg->num_entries > 0) {
        msg->entries = calloc(msg->num_entries, sizeof(paxos_entry_t));
        for (size_t i = 0; i < msg->num_entries; i++) {
            if (ptr + 21 > len) break;
            paxos_entry_t *e = &msg->entries[i];
            e->slot = dec64(buf + ptr); ptr += 8;
            e->accepted_ballot = dec64(buf + ptr); ptr += 8;
            e->type = buf[ptr++];
            e->data_len = dec32(buf + ptr); ptr += 4;

            if (e->data_len > 0 && ptr + e->data_len <= len) {
                e->data = malloc(e->data_len);
                memcpy(e->data, buf + ptr, e->data_len);
                ptr += e->data_len;
            }
        }
    }

    if (ptr + 12 <= len) {
        msg->snapshot_offset = dec64(buf + ptr); ptr += 8;
        msg->snapshot_len = dec32(buf + ptr); ptr += 4;
        if (msg->snapshot_len > 0 && ptr + msg->snapshot_len <= len) {
            msg->snapshot_data = malloc(msg->snapshot_len);
            memcpy(msg->snapshot_data, buf + ptr, msg->snapshot_len);
            ptr += msg->snapshot_len;
        }
    }
}

static void free_paxos_msg_payload(paxos_msg_t *msg) {
    if (msg->num_entries > 0 && msg->entries) {
        for (size_t i = 0; i < msg->num_entries; i++) {
            if (msg->entries[i].data) free(msg->entries[i].data);
        }
        free(msg->entries);
    }
    if (msg->snapshot_data) free(msg->snapshot_data);
}

// --- NETWORK FRAMING & I/O ---

static void on_write_done(uv_write_t *req, int status) {
    (void)status;
    free(req->data);
    free(req);
}

static void network_send_raw(peer_connection_t *conn, uint8_t msg_type, void *payload, size_t len) {
    if (!conn->is_connected) return;

    size_t frame_len = 4 + 1 + len; // [Length] [Type] [Payload]
    uint8_t *frame = malloc(frame_len);
    enc32(frame, (uint32_t)len);
    frame[4] = msg_type;
    if (len > 0) memcpy(frame + 5, payload, len);

    uv_buf_t buf = uv_buf_init((char*)frame, frame_len);
    uv_write_t *req = malloc(sizeof(uv_write_t));
    req->data = frame;

    uv_write(req, (uv_stream_t*)&conn->socket, &buf, 1, on_write_done);
}

static void p2p_handle_incoming_frame(peer_connection_t *conn, uint8_t msg_type, uint8_t *payload, size_t len) {
    if (msg_type == NET_MSG_PING) {
        network_send_raw(conn, NET_MSG_PONG, payload, len);
    }
    else if (msg_type == NET_MSG_PONG) {
        if (len < 8) return;
        uint64_t sent_us = dec64(payload);
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
        p2p_deserialize_msg(payload, len, &msg);

        paxos_err_t err = paxos_receive(conn->server->paxos, &msg);
        if (err != PAXOS_OK) {
            fprintf(stderr, "[P2P] Warning: Paxos engine rejected message from node %llu (Error: %d)\n",
                    (unsigned long long)msg.from, err);

            if (paxos_has_fatal_error(conn->server->paxos)) {
                fprintf(stderr, "FATAL: Paxos engine corrupted. Halting node.\n");
                uv_stop(&conn->server->paxos_loop);
            }
        }
        free_paxos_msg_payload(&msg);
    }
}

static void on_alloc(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    (void)handle;
    buf->base = malloc(suggested_size);
    buf->len = suggested_size;
}

// Cleanly free incoming anonymous sockets on disconnect
static void on_anon_close(uv_handle_t *handle) {
    peer_connection_t *conn = (peer_connection_t *)handle->data;
    if (conn->read_buf) free(conn->read_buf);
    free(conn);
}

// Reset outgoing fixed sockets so they can be retried
static void on_outgoing_close(uv_handle_t *handle) {
    peer_connection_t *conn = (peer_connection_t *)handle->data;
    conn->socket.type = UV_UNKNOWN_HANDLE; // Mark as safely closed
}

static void on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    peer_connection_t *conn = (peer_connection_t *)stream->data;

    if (nread < 0) {
        conn->is_connected = false;

        if (conn->peer_node_id == 0) {
            // It's an incoming anonymous socket. Free it entirely.
            uv_close((uv_handle_t*)stream, on_anon_close);
        } else {
            // It's a tracked outgoing connection. Mark closed to allow retry.
            uv_close((uv_handle_t*)stream, on_outgoing_close);
        }

        if (buf->base) free(buf->base);
        return;
    }

    if (nread > 0) {
        if (conn->read_pos + nread > conn->read_cap) {
            conn->read_cap = (conn->read_pos + nread) * 2 + 4096;
            conn->read_buf = realloc(conn->read_buf, conn->read_cap);
        }
        memcpy(conn->read_buf + conn->read_pos, buf->base, nread);
        conn->read_pos += nread;

        size_t processed = 0;
        while (conn->read_pos - processed >= 5) {
            uint32_t payload_len = dec32(conn->read_buf + processed);
            if (conn->read_pos - processed >= 5 + payload_len) {
                uint8_t msg_type = conn->read_buf[processed + 4];
                uint8_t *payload = conn->read_buf + processed + 5;

                p2p_handle_incoming_frame(conn, msg_type, payload, payload_len);
                processed += 5 + payload_len;
            } else {
                break; // Need more bytes to complete frame
            }
        }

        if (processed > 0) {
            memmove(conn->read_buf, conn->read_buf + processed, conn->read_pos - processed);
            conn->read_pos -= processed;
        }
    }

    if (buf->base) free(buf->base);
}

// --- CONNECTION LIFECYCLE ---

static void on_connect(uv_connect_t *req, int status) {
    peer_connection_t *conn = (peer_connection_t *)req->data;
    if (status < 0) {
        uv_close((uv_handle_t*)&conn->socket, on_outgoing_close);
        return;
    }
    conn->is_connected = true;
    uv_read_start((uv_stream_t*)&conn->socket, on_alloc, on_read);
}

static void try_connect(uv_timer_t *handle) {
    peer_connection_t *conn = (peer_connection_t *)handle->data;
    if (conn->is_connected || conn->peer_node_id == conn->server->opts.node_id) return;

    // Wait until libuv safely closes the old handle before trying again
    if (conn->socket.type != UV_UNKNOWN_HANDLE) return;

    struct sockaddr_in dest;
    uv_ip4_addr("127.0.0.1", 9000 + (int)conn->peer_node_id, &dest);

    uv_tcp_init(&conn->server->paxos_loop, &conn->socket);
    conn->socket.data = conn;
    uv_tcp_connect(&conn->connect_req, &conn->socket, (const struct sockaddr*)&dest, on_connect);
}

static void on_ping_timer(uv_timer_t *handle) {
    peer_connection_t *conn = (peer_connection_t *)handle->data;
    uint64_t now_us = uv_hrtime() / 1000;
    uint8_t buf[8];
    enc64(buf, now_us);
    network_send_raw(conn, NET_MSG_PING, buf, 8);
}

static void on_accept(uv_stream_t *server, int status) {
    if (status < 0) return;

    // PRODUCTION FIX: Initialize the socket inside the connection struct
    peer_connection_t *anon_conn = calloc(1, sizeof(peer_connection_t));
    anon_conn->server = ((peer_connection_t*)server->data)->server;
    anon_conn->is_connected = true;
    anon_conn->read_cap = 4096;
    anon_conn->read_buf = malloc(4096);

    uv_tcp_init(server->loop, &anon_conn->socket);
    anon_conn->socket.data = anon_conn;

    if (uv_accept(server, (uv_stream_t*)&anon_conn->socket) == 0) {
        uv_read_start((uv_stream_t*)&anon_conn->socket, on_alloc, on_read);
    } else {
        uv_close((uv_handle_t*)&anon_conn->socket, on_anon_close);
    }
}

// --- PUBLIC P2P API ---

void p2p_network_init(paxos_server_t *s) {
    for (int i = 1; i <= 3; i++) {
        g_peers[i].peer_node_id = i;
        g_peers[i].server = s;
        g_peers[i].is_connected = false;
        g_peers[i].socket.type = UV_UNKNOWN_HANDLE; // Mark clean for first connect
        g_peers[i].read_cap = 4096;
        g_peers[i].read_buf = malloc(4096);
        g_peers[i].read_pos = 0;

        g_peers[i].connect_req.data = &g_peers[i];

        uv_timer_init(&s->paxos_loop, &g_peers[i].reconnect_timer);
        g_peers[i].reconnect_timer.data = &g_peers[i];
        uv_timer_start(&g_peers[i].reconnect_timer, try_connect, 0, 1000);

        uv_timer_init(&s->paxos_loop, &g_peers[i].ping_timer);
        g_peers[i].ping_timer.data = &g_peers[i];
        uv_timer_start(&g_peers[i].ping_timer, on_ping_timer, 500, 500);
    }

    uv_tcp_init(&s->paxos_loop, &g_listener);
    g_listener.data = &g_peers[1];

    struct sockaddr_in addr;
    uv_ip4_addr("0.0.0.0", s->opts.p2p_port, &addr);
    uv_tcp_bind(&g_listener, (const struct sockaddr*)&addr, 0);
    uv_listen((uv_stream_t*)&g_listener, 128, on_accept);
}

int p2p_network_get_latency(paxos_server_t *s, uint64_t node_id) {
    (void)s;
    if (node_id < 1 || node_id > 3) return -1;
    if (!g_peers[node_id].is_connected) return -1;
    return (int)g_peers[node_id].smoothed_latency_ms;
}

void p2p_network_send(paxos_server_t *s, uint64_t to_node, paxos_msg_t *msg) {
    (void)s;
    if (to_node < 1 || to_node > 3) return;
    peer_connection_t *conn = &g_peers[to_node];
    if (!conn->is_connected) return;

    size_t len;
    uint8_t *buf = p2p_serialize_msg(msg, &len);
    network_send_raw(conn, NET_MSG_PAXOS, buf, len);
    free(buf);
}
