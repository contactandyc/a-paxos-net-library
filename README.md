# a-paxos-net-library

**The high-performance, asynchronous network and event-loop adapter for C-based distributed state machines.**

`a-paxos-net-library` is the "Glue" layer that bridges the mathematical purity of `a-paxos-core` with the physical realities of disk I/O, network latency, and client HTTP traffic. It transforms a strict, single-threaded consensus engine into a fully linearizable, globally distributed database daemon.

## 🏗️ Architecture

This library enforces a strict **Separation of Concerns** between the math (consensus), the physics (networking/timers), and the application (storage). It utilizes a highly optimized **Multi-Reactor Architecture** powered by `libuv` and `h2o`:

* **The Consensus Reactor (Data Plane):** A dedicated OS thread running a raw TCP P2P event loop. It ticks the Paxos engine, exchanges binary-framed RPCs with peers, executes the strict 3-step Consensus WAL persistence protocol, and applies committed state to the storage engine.
* **The HTTP Reactors (Client API):** A pool of lock-free `h2o` worker threads that parse incoming REST traffic (`GET`, `PUT`, `DELETE`).

### Core Features

* **Flawless Asynchronous Bridging:** HTTP worker threads never block waiting for consensus. Write requests are queued to the Consensus thread, and responses are safely dispatched back across thread boundaries via `uv_async_t` only *after* a global disk quorum is achieved.
* **Resilient Sync Manager:** Built-in support for **Cascading Replication**. Learners intelligently catch up by pulling SSTables and log chunks from other healthy Learners rather than overwhelming the Leader. Connections are fully resumable; if a peer dies during a 10GB transfer, the transfer instantly resumes from the exact byte offset on a different peer.
* **EWMA Latency Tracking:** Uses dedicated `PING`/`PONG` frames to calculate the Exponentially Weighted Moving Average of network latency between peers, allowing the Sync Manager to intelligently route bulk data transfers around congested network links.
* **Control Plane / Data Plane Isolation:** The library exposes APIs for external orchestrators (like Kubernetes operators) to safely execute Joint-Consensus node swaps (`paxos_swap_node`) without risking split-brain scenarios or auto-demotion traps.

---

## 🚀 Quick Start & Integration

`a-paxos-net-library` is entirely agnostic to your storage engine. You plug your application logic in via the `paxos_app_machine_t` interface.

```c
#include "a-table-store-library/lsm_env.h"
#include "a-paxos-net-library/paxos_net.h"

// 1. Define how committed state is applied to your application
void on_paxos_apply(void *ctx, uint64_t slot, uint8_t op, const void *key, uint32_t klen, const void *val, uint32_t vlen) {
    lsm_db_t *db = (lsm_db_t*)ctx;
    if (op == 0 /* PUT */) lsm_db_put(db, key, klen, val, vlen);
    else lsm_db_delete(db, key, klen);
}

// 2. Define read-only routing (Bypasses Paxos, routes to H2O)
void on_http_get(void *ctx, h2o_c_req_t *req, const char *path) {
    lsm_db_t *db = (lsm_db_t*)ctx;
    // ... query LSM ...
    h2o_c_response_t *resp = h2o_c_make_response(200, "OK", data, len, "text/plain");
    h2o_c_send_response(req, resp);
}

int main() {
    // Initialize your storage engine
    lsm_env_t *env = lsm_env_init(...);
    lsm_db_t *db = lsm_db_open(env, ...);

    // Bind the App Interface
    paxos_app_machine_t app = {
        .app_ctx = db,
        .on_apply = on_paxos_apply,
        .on_http_get = on_http_get,
        // ... snapshot callbacks
    };

    // Configure Network and Cluster Topology
    paxos_config_t pcfg = { .node_id = 1, .initial_voters = {1, 2, 3}, ... };
    paxos_server_options_t ncfg = {
        .node_id = 1, 
        .p2p_port = 9091, 
        .tick_ms = 10,
        .h2o = { .port = 8081, .thread_pool_size = 4, .address = "0.0.0.0" }
    };

    // Ignite the Server
    paxos_server_t *server = paxos_server_init(&pcfg, &app, &ncfg);
    paxos_server_run(server); // Blocks, multiplexing P2P and HTTP traffic

    return 0;
}

```

---

## 📂 Project Structure

| File | Responsibility |
| --- | --- |
| `include/.../paxos_net.h` | The public API and application boundary. |
| `src/paxos_net_server.c` | The core Daemon. Manages the libuv loop, thread-safe command queues, and the strict 3-step commit protocol. |
| `src/p2p_network.c` | Handles raw TCP socket lifecycle, binary framing, and physical EWMA latency tracking. |
| `src/sync_manager.c` | The resilient catch-up state machine. Uses core metadata to select optimal peers for data streaming. |

---

## 🛠️ Building

This project relies on `CMake` and `Ninja`.

### Dependencies

* `libuv` (Asynchronous I/O)
* `h2o` (Optimized HTTP server)
* `a-paxos-core` (The pure consensus math engine)

```bash
mkdir build && cd build
cmake -G Ninja ..
ninja
```
