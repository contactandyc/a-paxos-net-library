// SPDX-FileCopyrightText: 2026 Andy Curtis <contactandyc@gmail.com>
// SPDX-License-Identifier: Apache-2.0
//
// Maintainer: Andy Curtis <contactandyc@gmail.com>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <signal.h>
#include <errno.h>

#include "a-paxos-net-library/paxos_net.h"
#include "the-macro-library/macro_test.h"

#define TEST_HTTP_PORT 9080
#define TEST_P2P_PORT 9090
#define TEST_HOST "127.0.0.1"

/* --- 1. Mock Application State (The "Storage Engine") --- */

typedef struct {
    int apply_count;
    char last_key[256];
    char last_val[256];
    uint8_t last_op;
} mock_app_state_t;

static mock_app_state_t g_mock_app = {0};
static paxos_server_t *g_server = NULL;

// Callback: Paxos has committed a write. Apply it to our mock state.
static void mock_on_apply(void *ctx, uint64_t slot, uint8_t op, const void *key, uint32_t klen, const void *val, uint32_t vlen) {
    (void)slot;
    mock_app_state_t *app = (mock_app_state_t *)ctx;

    app->apply_count++;
    app->last_op = op;

    if (klen < sizeof(app->last_key)) {
        memcpy(app->last_key, key, klen);
        app->last_key[klen] = '\0';
    }

    if (vlen > 0 && val && vlen < sizeof(app->last_val)) {
        memcpy(app->last_val, val, vlen);
        app->last_val[vlen] = '\0';
    } else {
        app->last_val[0] = '\0';
    }
}

// Callback: HTTP thread received a GET request.
static void mock_on_http_get(void *ctx, h2o_c_req_t *req, const char *path) {
    mock_app_state_t *app = (mock_app_state_t *)ctx;
    (void)path; // Ignoring actual route parsing for the mock

    h2o_c_response_t *resp = h2o_c_make_response(200, "OK", app->last_val, strlen(app->last_val), "text/plain");
    h2o_c_send_response(req, resp);
}

// Dummy snapshot callbacks
static void mock_on_snapshot_create(void *ctx, uint64_t *out_idx) { (void)ctx; *out_idx = 0; }
static void mock_on_snapshot_chunk_recv(void *ctx, uint64_t s, uint64_t o, const uint8_t *d, size_t l, bool f) {
    (void)ctx; (void)s; (void)o; (void)d; (void)l; (void)f;
}


/* --- 2. Client Helper --- */

static char *client_send(const char *raw_request, size_t req_len, int *out_len) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return NULL;

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(TEST_HTTP_PORT);
    inet_pton(AF_INET, TEST_HOST, &serv_addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sock);
        return NULL;
    }

    size_t sent = 0;
    while(sent < req_len) {
        ssize_t n = write(sock, raw_request + sent, req_len - sent);
        if (n <= 0) break;
        sent += n;
    }

    char *resp = calloc(1, 16384);
    size_t total_read = 0;

    // Wait patiently. Paxos ticks every 10ms, so consensus might take 10-30ms.
    for(int i = 0; i < 100; i++) {
        ssize_t n = read(sock, resp + total_read, 16383 - total_read);
        if (n > 0) {
            total_read += n;
            usleep(10000);
        } else if (n == 0) {
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(10000);
                continue;
            }
            break;
        }
    }

    close(sock);
    if (total_read == 0) { free(resp); return NULL; }
    if (out_len) *out_len = (int)total_read;
    return resp;
}


/* --- 3. Integration Tests --- */
MACRO_TEST(test_pipeline_put_consensus) {
    const char req[] = "PUT /key/test_key HTTP/1.1\r\nContent-Length: 11\r\nConnection: close\r\n\r\nhello_world";

    int len = 0;
    char *resp = client_send(req, strlen(req), &len);
    MACRO_ASSERT_TRUE(resp != NULL);

    if (strstr(resp, "HTTP/1.1 200 OK") == NULL) {
        printf("\n[ERROR] Expected 200 OK, but got:\n%s\n", resp);
    }
    MACRO_ASSERT_TRUE(strstr(resp, "HTTP/1.1 200 OK") != NULL);

    // 2. Verify the Paxos thread successfully executed the command
    MACRO_ASSERT_EQ_INT(g_mock_app.apply_count, 1);
    MACRO_ASSERT_EQ_INT(g_mock_app.last_op, 0); // 0 = PUT

    // 3. Verify the payload survived the serialization boundaries
    MACRO_ASSERT_TRUE(strcmp(g_mock_app.last_key, "test_key") == 0);
    MACRO_ASSERT_TRUE(strcmp(g_mock_app.last_val, "hello_world") == 0);

    free(resp);
}

MACRO_TEST(test_pipeline_read_bypass) {
    // Send a GET request. This should bypass Paxos and read the state we just wrote.
    const char req[] = "GET /key/test_key HTTP/1.1\r\nConnection: close\r\n\r\n";

    int len = 0;
    char *resp = client_send(req, strlen(req), &len);
    MACRO_ASSERT_TRUE(resp != NULL);

    // Verify H2O served the payload from our mock app state
    MACRO_ASSERT_TRUE(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    MACRO_ASSERT_TRUE(strstr(resp, "hello_world") != NULL);

    free(resp);
}

/* --- 4. Test Runner Lifecycle --- */

static void *server_bg_thread(void *arg) {
    (void)arg;
    paxos_server_run(g_server);
    return NULL;
}

int main(void) {
    // Crucial: Prevent SIGPIPE from killing the test suite on closed sockets
    signal(SIGPIPE, SIG_IGN);

    // Initialize 1-Node Quorum
    uint64_t initial_voters[] = {1};
    paxos_config_t pcfg = {
        .struct_size = sizeof(paxos_config_t),
        .node_id = 1,
        .initial_voters = initial_voters,
        .num_initial_voters = 1,
        .heartbeat_ticks = 10,
        .election_ticks = 30,
        .max_payload_size = 4096,
        .max_batch_bytes = 4096
    };

    paxos_app_machine_t app = {
        .app_ctx = &g_mock_app,
        .on_apply = mock_on_apply,
        .on_http_get = mock_on_http_get,
        .on_snapshot_create = mock_on_snapshot_create,
        .on_snapshot_chunk_recv = mock_on_snapshot_chunk_recv
    };

    paxos_server_options_t nopts = {
        .node_id = 1,
        .p2p_port = TEST_P2P_PORT,
        .tick_ms = 10,
        .h2o = {
            .port = TEST_HTTP_PORT,
            .thread_pool_size = 2,
            .address = TEST_HOST,
            .enable_http2 = false,
            .enable_ssl = false
        }
    };

    g_server = paxos_server_init(&pcfg, &app, &nopts);
    if (!g_server) {
        fprintf(stderr, "Failed to initialize paxos_server\n");
        return 1;
    }

    // Launch Daemon
    pthread_t tid;
    pthread_create(&tid, NULL, server_bg_thread, NULL);

    // Give H2O a moment to bind ports and Paxos a moment to elect itself leader
    usleep(1000000);
    // Run Suite
    macro_test_case tests[64];
    size_t test_count = 0;

    MACRO_ADD(tests, test_pipeline_put_consensus);
    MACRO_ADD(tests, test_pipeline_read_bypass);

    macro_run_all("paxos_net_integration", tests, test_count);

    // Teardown
    paxos_server_stop(g_server);
    pthread_join(tid, NULL);
    paxos_server_destroy(g_server);

    return 0;
}
