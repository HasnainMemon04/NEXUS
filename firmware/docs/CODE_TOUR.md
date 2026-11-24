# NEXUS code tour

A guided walk through the firmware for the team. Read it once from top to bottom with
the code open next to it; after that every module should feel familiar.

Everything is plain C on ESP-IDF 6.1 / FreeRTOS. Each file holds one OS idea, and every
place where an OS primitive is used is marked with an `OS CONCEPT` comment and the
Silberschatz (OSC 10e) section it illustrates.

The final deployment uses a dual-core ESP32-S3-N16R8 master and three dual-core classic ESP32 workers.
See the [proposal checklist](PROPOSAL_CHECKLIST.md) for acceptance criteria and
the [firmware README](../README.md) for the role-specific builds.

---

## 1. Reading order

1. `components/nexus_core/include/nexus_config.h`: every number in the system (capacities, priorities, timeouts).
2. `components/nexus_core/include/nexus_types.h`: what a job and a result look like.
3. `components/nexus_core/job_queue.c`: the shared job queue (M1). **The most important file.**
4. `components/nexus_core/exec_pool.c`: the thread pool and execution slots (M2, M3).
5. `components/nexus_core/kernels.c`: what a job actually computes.
6. `master/main/cluster.c`: how the master spreads jobs over the boards and survives failures.
7. `worker/main/worker_link.c`: the worker's side of the same conversation.
8. Everything else (logger, monitor, lab, stats, web, experiments) adds observation and measurement.

---

## 2. The life of one job

Follow a single fractal tile from the moment it is created until it appears on the screen.

| # | What happens | Where | OS idea |
|---|---|---|---|
| 1 | The frame producer creates 150 tile jobs for a new image | `producers.c` `submit_frame()` | producers |
| 2 | Each job enters the global queue: `take(sem_free)` → lock `q_mtx` → insert → unlock → `give(sem_ready)` | `job_queue.c` `jq_submit()` | bounded buffer, mutex, counting semaphores |
| 3 | The dispatcher wakes up (it was blocked on `sem_ready`) | `cluster.c` `dispatcher_task()` → `jq_wait_ready()` | consumer sleeps while the buffer is empty |
| 4 | It picks a board with a free credit (placement policy) | `cluster.c` `choose_node()` | load balancing |
| 5 | It removes the best job for the ordering policy and records it in flight with a lease | `job_queue.c` `jq_pop_best()` | scheduling, aging |
| 6 | A DISPATCH message is queued for that worker's single sender task | `cluster.c` `send_dispatch()` → `link_tx_task()` | message passing |
| 7 | The worker's `link_rx` task receives it and pushes it into the worker's local queue, then queues an ACK | `worker_link.c` `link_rx_task()` | the worker's own bounded buffer |
| 8 | An idle executor takes `sem_ready`, then an execution permit from `sem_slots`, then pops the job | `exec_pool.c` `executor_task()` | thread pool, counting semaphore |
| 9 | The kernel computes 40×40 pixels, checking its time budget after every row | `kernels.c` `run_fractal()` | cooperative cancellation (timeouts) |
| 10 | The executor returns its permit and hands the result to `link_tx`, which sends RESULT | `exec_pool.c`, `worker_link.c` | back-pressure through `tx_queue` |
| 11 | The master's `link_rx` receives RESULT and commits it exactly once | `cluster.c` `on_result()` → `job_queue.c` `jq_commit()` | duplicate rejection |
| 12 | Statistics are recorded and the tile is copied into the frame image | `stats.c` `nx_stats_on_commit()`, `producers.c` `frame_on_tile()` | performance statistics |
| 13 | The telemetry task pushes the new state over the WebSocket; the browser fetches the tile and paints it | `web.c` `telemetry_task()`, `www/index.html` `fetchTiles()` | — |

If the worker dies between steps 6 and 11, the supervisor notices that heartbeats have stopped,
marks the worker OFFLINE and calls `jq_requeue_node()`. The job goes back to step 3 with
`attempt + 1`.

---

## 3. Shared component: `components/nexus_core/`

### `nexus_config.h`
All tunables. Capacities (`NX_QUEUE_CAPACITY` 256, `NX_POOL_SIZE` 4, `NX_DEFAULT_SLOTS` 2),
time limits (heartbeat 500 ms, SUSPECT 1.5 s, OFFLINE 3 s, aging step 1.5 s), task priorities and
stack sizes. Change behaviour here, not inside the code.

### `nexus_types.h`
- `nx_job_spec_t` is a job as it travels: id, attempt, type, base priority, execution budget, and
  parameters (fractal view, matrix size and seed, or spin time). It carries **no input data**; the
  worker generates the input itself, so messages stay small.
- `nx_result_t` is the reply: status (OK / TIMEOUT / ERROR), the core it started on, execution time
  measured on the worker, CRC32 of the output, and for tiles the pixel count (`data_len`, 1600).
- These structs are `packed` because they cross the network. Never take a pointer to one of their
  fields; copy into a local variable first. The compiler rejects the pointer, and the code
  shows the pattern in several places.

### `job_queue.c`: the shared job queue (M1)
- `ready[]` is the bounded buffer of waiting jobs. `inflight[]` is used on the master only and
  holds jobs that were dispatched but not yet committed.
- One mutex `mtx` (called q_mtx in the docs) protects both arrays and all counters, so a job is
  always in exactly one place.
- `sem_free` counts free places (producers wait on it); `sem_ready` counts waiting jobs
  (consumers wait on it).
- `jq_submit()` is the producer side; `jq_wait_ready()` + `jq_pop_best()` is the consumer side.
  The consumer is split in two steps so an executor can take an execution permit between them.
- `jq_effective_priority()` holds the three ordering policies. FCFS returns 0 for every job, so
  the oldest sequence number wins. Strict priority returns the base priority. Aging adds +1 for
  every 1.5 s of waiting.
- `pick_best_locked()` scans the waiting jobs once (at most 256) and keeps the best one.
- Master-only functions:
  - `jq_mark_running()`: an ACK arrived.
  - `jq_commit()`: the first valid result wins; later ones are duplicates. A late result for a job that was already requeued still commits, and the queued copy is cancelled lazily.
  - `jq_requeue_one()`, `jq_requeue_node()`, `jq_requeue_expired()`: give a job back to the queue, or mark it FAILED after 3 attempts. These never block: if the buffer is full the job is flagged `requeue_pending` and `jq_retry_pending()` tries again.
  - `jq_cancel_old_frames()`: when the user zooms, old tiles are marked CANCELLED and dropped when popped (lazy deletion, so the semaphore counts stay right).
- `q_lock()` first tries without waiting so it can count contention (shown on the dashboard).

### `exec_pool.c`: thread pool + execution slots (M2, M3)
- `pool_start()` creates `exec_0..exec_3` once at boot. No task is ever created per job.
- `executor_task()`: `take(sem_ready)` → `take(sem_slots)` → `jq_pop_best()` → run → return the permit → callback.
- **K permits, N tasks.** `sem_slots` holds K ≤ N permits. Tasks without a permit stay blocked,
  so at most K jobs run. `peak_running` proves it.
- `pool_set_slots()` changes K live: extra permits are given at once, and surplus permits are
  *retired* when they come back (`return_permit()`). A running job is never interrupted.
- A spinlock (`portMUX`, `taskENTER_CRITICAL`) protects the small counters, because they
  are touched for a few instructions only.
- The executor sleeps one tick at least once per second. Without that, two CPU-bound executors
  could keep both idle tasks from ever running, and the task watchdog would complain after 5 s.

### `kernels.c`: the jobs
- `run_fractal()`: Mandelbrot iteration in `float` (the ESP32-S3 has a single-precision FPU). Each pixel becomes one byte: 255 = inside the set, otherwise the iteration count scaled to 0..253.
- `run_matmul()`: n×n integer matrices generated from a seed (xorshift), result CRC32.
- `run_spin()`: busy work for an exact time (synthetic, for controlled tests).
- Every kernel checks `deadline_us` regularly and returns `NX_RES_TIMEOUT` when the budget is used up. This is cooperative cancellation: FreeRTOS cannot safely kill a task in the middle of its work.

### `logger.c`: logging thread (A4)
`nx_log()` formats a record and does `xQueueSend(..., 0)`. If the 64-entry queue is full the record is
dropped and counted, so logging can never block the dispatcher or an executor. The `logger` task prints
records and keeps the last 64 for the dashboard.

### `monitor.c`: FreeRTOS task monitor (A4)
`monitor_sample()` calls `uxTaskGetSystemState()` and, for every task, reports:
- state;
- priority;
- core affinity (`xTaskGetCoreID`: the core it is pinned to, −1 = either);
- minimum free stack;
- CPU share, from the difference of the run-time counter between two samples.

Per-core load is 100 − idle-task share. These are **samples**, not a trace.

### `proto.c` / `proto.h`: messages between boards
- An 8-byte header `{magic "NX", version, type, length}` followed by the payload.
- `nx_recv_frame()` reads exactly the header, checks it, then reads exactly `length` bytes, however TCP splits the stream.
- Message types: HELLO, WELCOME, DISPATCH, ACK, RESULT, HEARTBEAT, CONTROL.
- The heartbeat carries progress (running job ids, free slots, completed count), core load, the task table and the lab results.

### `oslab.c`: synchronization laboratory
- **A:** two tasks pinned to core 0 and core 1 do `v = counter; v = v + 1; counter = v;` 100 000 times each with no lock, so updates get lost.
- **B:** the same with a mutex.
- **C:** six tasks share a counting semaphore with two permits; the peak number of holders is recorded.
- **D:** priority inversion. L, M and H are all pinned to core 1:
  1. L takes the lock.
  2. H blocks on it.
  3. CPU-bound M starts.
  4. With a binary semaphore, H waits for M; with a mutex, L inherits H's priority and finishes first.
  5. H's waiting time is measured in both cases.

  The runner task sits on core 0 so the experiment cannot block it.

---

## 4. Master app: `master/main/`

| File | Responsibility |
|---|---|
| `main.c` | Start order: NVS, logger, lab, stats, global queue, Wi-Fi AP, cluster, producers, experiments, web |
| `wifi_ap.c` | SoftAP `NEXUS-OS` at 192.168.4.1, max 6 stations, WPA2 |
| `cluster.c` | Node table, listener, link tasks, dispatcher, supervisor, master monitor, controls |
| `producers.c` | Client tasks, fractal frames and auto-zoom, frame store and image CRC |
| `stats.c` | Ring of the last 4096 commits with per-job checksums, KPIs, CSV, per-run summaries |
| `experiments.c` | Test suites T1–T5 |
| `web.c` | HTTP + WebSocket server, JSON state, tiles, commands, CSV |
| `www/index.html` | The NEXUS Control Center (gzipped into flash by `main/CMakeLists.txt`) |

### `cluster.c`, the heart of the master
- **Node table.** `s_nodes[0]` is the master's own engine (single-node mode), `[1]` is
  Worker A, `[2]` is Worker B, and `[3]` is Worker C. A worker is identified by **MAC address**, and its reboots by the
  **incarnation** number in HELLO. Its IP address is only displayed.
- **Locking rule.** `nodes_mtx` (node table) and `q_mtx` (job queue) are **never held at the
  same time**. Code copies what it needs, releases `nodes_mtx`, and only then calls `jq_*()`.
  That removes hold-and-wait between the two locks, so they cannot deadlock (OSC §8.5).
- **`listener_task`.** `accept()`, read HELLO with a 2 s timeout, then `admit()`:
  1. find the slot by MAC (or a free slot);
  2. reset its `tx_queue` and record the new socket (under `nodes_mtx`);
  3. after releasing that lock, if the incarnation changed, requeue that worker's old jobs, and record a failure if it had not been declared OFFLINE yet;
  4. queue a WELCOME;
  5. hand the socket to that slot's `link_rx` task with `xTaskNotify`.
- **`link_rx_A/B/C`** receive frames. Any valid frame refreshes `last_heard`. HEARTBEAT stores the
  progress and task table, ACK marks the job running, and RESULT goes to `on_result()`, which requeues
  TIMEOUT or ERROR results and otherwise stores the tile first, then commits. When the
  connection breaks it closes the socket and calls `go_offline()`.
- **`link_tx_A/B/C`** are the only senders on each socket. Everything for a worker goes through
  its `tx_queue`, so messages can never interleave.
- **`dispatcher_task`.** Wait for a job (`sem_ready`), wait for a node with a credit
  (`choose_node()`), pop the best job, send it. A node has a credit when fewer than K + 1 jobs
  are in flight on it.
- **`supervisor_task`.** Runs every 100 ms:
  - no message for 1.5 s → SUSPECT;
  - no message for 3 s → `go_offline()`, which requeues the node's jobs and records the detection delay;
  - expired leases are requeued;
  - pending requeues are retried.
- **`monitor_task`.** Samples the master's own tasks into node 0's heartbeat slot, so the
  dashboard gets the same telemetry format from all four boards, with the active core count for each.

### `producers.c`
- `client_task` × 4 (only the first `s_clients` are active). Each submits SPIN or MATMUL jobs
  with random priority, blocking when the queue is full; the "storm" mode sends P3 jobs fast.
- `frame_task` submits the first frame, serves zoom requests, and does auto-zoom 2.5 s after a frame completes.
- Tile priority = distance from the image centre (P3 in the middle, P0 in the corners).
- `frame_on_tile()` copies a committed tile into the 600×400 image (PSRAM). The first copy of a
  tile wins, so duplicates cannot overwrite. When all 150 tiles are in, the image CRC32 is computed.

### `stats.c`
Each commit stores latency (done − submit) and queue wait (dispatch − submit), both on the **master's
clock**, plus node, attempt, run id and the execution time reported by the worker (its own clock). The
ring keeps the last 4096 commits. `nx_stats_view()` gives the dashboard KPIs;
`nx_stats_window()` summarises one experiment run; `nx_stats_csv()` streams `/api/csv`.

### `web.c`
- **`build_state()`** collects one view from every module, each under its own lock and one at a time, then writes compact JSON.
- **`telemetry_task`** builds the state at most four times per second during jobs and twice per second while idle, when browser clients are connected. The HTTP task sends it with at most one broadcast queued. The browser pages through completed tiles in batches of eight using the version watermark.
- **Endpoints:** `/`, `/ws`, `/api/state`, `/api/tiles?since=`, `/api/cmd?op=…`, `/api/csv`, `/api/runs.csv`.
- **Commands** (`op=`): `ord`, `plc`, `k`, `drain`, `local`, `cli`, `storm`, `runaway`, `zoom`, `auto`, `mute`, `reboot`, `lab`, `exp`, `expstop`.

### `experiments.c`
Runs a suite as a sequence of runs. For each run it:
1. applies a configuration (ordering, placement, K, which boards);
2. waits until the cluster is idle;
3. tags commits with a run id and submits a fixed workload;
4. waits until every job is committed;
5. stores a summary.

Client tasks and auto-zoom are paused during a suite, and the master refuses every other dashboard
control, so every commit belongs to the run. Stop ends a suite even when no board is accepting work.
Summaries go to `/api/runs.csv`; `/api/csv` keeps the last 4096 committed jobs.

| Suite | Runs | Workload | What it shows |
|---|---|---|---|
| T1 | 3 orderings × 2 placements × 3 | frame + 60 mixed-priority SPIN | per-priority waits, starvation vs aging |
| T2 | reference + 1 | frame (+ 4 producer tasks × 40 jobs) | the image CRC must equal the single-node reference |
| T3 | reference + 1 | frame, Worker A crashed at 40 % | detection delay, recovery, same CRC |
| T4 | K = 1..4 on A/B/C × 3 | 120 MATMUL n = 32 | compare throughput and latency when permits match, underuse, or exceed each worker's two cores |
| T5 | {0,1,2,3 workers} × {n = 12, 32} × 3 | 100 MATMUL | compare the dual-core master alone with one to three dual-core workers |

---

## 5. Worker app: `worker/main/`

| File | Responsibility |
|---|---|
| `main.c` | NVS, logger, lab, local queue (8 places), pool (N = 4, boot K = 2 on these boards), Wi-Fi, link |
| `wifi_sta.c` | Join the 20 MHz `NEXUS-OS` access point with power saving off. Retry association with jitter and use the DHCP gateway as the master's IP |
| `worker_link.c` | `link_rx` bounds TCP connection time and retries with backoff and jitter. It sends HELLO and receives DISPATCH and CONTROL. `link_tx` sends results, ACKs, and heartbeats and closes a link when its station address changes |

CONTROL commands the worker understands:
- SET_SLOTS: change K.
- MUTE_LINK: stop sending for N ms (a simulated network fault). Results wait in `tx_queue` and arrive late, which tests duplicate handling.
- REBOOT: `esp_restart()`, a simulated crash.
- RUN_LAB: run a sync-lab experiment.

---

## 6. All tasks and their priorities

Larger number = more urgent. ESP-IDF's own tasks: Wi-Fi 23 (core 0), esp_timer 22, lwIP `tiT` 18, idle tasks 0.

| Task | Board | Prio | Blocks on |
|---|---|---|---|
| `supervisor` | master | 11 | delay 100 ms |
| `link_rx_A/B`, `link_tx_A/B` | master | 10 | socket recv / `tx_queue` |
| `link_rx`, `link_tx` | worker | 10 | socket recv / `tx_queue` (500 ms timeout = heartbeat) |
| `listener` | master | 9 | `accept()` |
| `lab_run` (while a lab runs, pinned to core 0) | any | 9 | `s_done` (completion of the lab tasks) |
| `dispatcher` | master | 8 | `sem_ready`, then credits |
| `telemetry` | master | 6 | delay 100 ms |
| `httpd` (ESP-IDF) | master | 5 | sockets |
| `exec_0..3` | all | 5 | `sem_ready`, then `sem_slots` |
| `client_0..3`, `frames`, `experiments` | master | 4 | delay / `sem_free` when the queue is full |
| `monitor` | master | 4 | delay 500 ms |
| `burst` ×4 (during T2 only) | master | 4 | `sem_free` when the queue is full |
| `lab_t0/t1`, `lab_c` ×6, `lab_L/M/H` (during a lab only) | any | 4, 4, 6/7/8 | lab semaphores (L/M/H pinned to core 1) |
| `logger` | all | 3 | `log_queue` |

On a worker there is no separate monitor task: `link_tx` samples the task table (`monitor_sample()`)
each time it sends a heartbeat.

Why this order: messages and supervision must never wait behind computation. If executors had a
higher priority than the link tasks, a busy worker would stop sending heartbeats and be declared
dead while it is actually working.

---

## 7. Synchronization objects

| Object | Type | Protects / counts | Taken by | Given by |
|---|---|---|---|---|
| `q_mtx` (in each `nx_job_queue_t`) | mutex | ready buffer, in-flight table, counters | every `jq_*` function | the same function |
| `sem_free` | counting, max = capacity | free places in the buffer | producers (`jq_submit`), requeue | consumers (`jq_pop_best`) |
| `sem_ready` | counting | waiting jobs | consumers (dispatcher, executors) | producers, requeue |
| `sem_slots` | counting, max N | execution permits (K) | executors before a job; `pool_set_slots` (retiring idle permits) | executors after a job (`return_permit`), `pool_set_slots` |
| `nodes_mtx` | mutex | master node table | cluster tasks, web views | the same |
| `tx_queue` (per link) | FreeRTOS queue | messages for one socket | link_tx | master: dispatcher, controls, `admit()` (WELCOME); worker: executors (RESULT), `link_rx` (ACK) |
| `log_queue` | FreeRTOS queue | log records | logger task | any task (non-blocking) |
| frame `s_fmtx` | mutex | frame image and tile versions | frame producer, commits, web | the same |
| stats `s_mtx` | mutex | commit ring and counters | commits, web, experiments | the same |
| pool `lock` | spinlock | running counters | executors, `pool_set_slots`, `pool_running_ids` | the same |

Rule: never hold two of the big mutexes at once (`q_mtx`, `nodes_mtx`, `s_fmtx`, stats `s_mtx`).
Every module takes its own lock, copies, and releases before calling another module. The only nesting
is an outer lock that is never taken in the other order: `web.c`'s build mutex wraps the per-module
views, and the master monitor reads the pool spinlock while holding `nodes_mtx`. That order is acyclic,
so it cannot deadlock.

---

## 8. Common changes

- **Different numbers** (pool size, timeouts, priorities): `nexus_config.h`. Rebuild **both** apps.
- **Wi-Fi name or password:** `idf.py menuconfig` → NEXUS, in both apps.
- **A new job type:**
  1. add it to `nx_job_type_t`;
  2. add a `static run_…()` in `kernels.c` **and** a `case` for it in `nx_run_job()` (otherwise the job
   returns ERROR and is retried until FAILED);
  3. produce it in `producers.c` or `experiments.c`;
  4. show its name in `www/index.html` (`TYPE`).
- **A new dashboard field:**
  1. add it to `build_state()` in `web.c`;
  2. read it in `applyState()` in `www/index.html`.

  The page is re-gzipped automatically at the next build.
- **A new experiment:**
  1. add a `case` in `run_suite()`;
  2. add a button with `data-exp="6"` in the page;
  3. allow 6 in `exp_request()`.
