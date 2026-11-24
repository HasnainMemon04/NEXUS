# NEXUS viva guide

Likely questions with short answers that every team member should be able to give, plus the
10-minute demo script. Answers point to the code (`file → function`) so you can open it on the
spot. Numbers such as "3 s" are **configured values** from `nexus_config.h`; measured results
come only from your experiment runs.

---

## A. The project

The course baseline is the submitted NEXUS v2.1 proposal. The final hardware is an
ESP32-S3-N16R8 master, two DevKit workers, and a WROOM-32U worker. Each board uses two FreeRTOS
cores. See the [proposal checklist](PROPOSAL_CHECKLIST.md) for physical test results to capture.

**1. What is NEXUS in one sentence?**
A multithreaded task orchestration engine in C on FreeRTOS: producers put jobs into a shared
bounded queue, a fixed pool of worker threads executes them under a semaphore limit, and a master
board schedules jobs across three worker boards and recovers when one fails.

**2. Did you write an operating system?**
No. FreeRTOS (inside ESP-IDF) is the operating system: it provides tasks (threads), the scheduler,
queues, mutexes and semaphores. NEXUS is an application-level engine built on those primitives,
like a small cloud orchestrator.

**3. How does it meet Project #2?**

| Requirement | How NEXUS meets it |
|---|---|
| M1 | Shared queue with a mutex: `job_queue.c`. |
| M2 | Fixed pool: `exec_pool.c → pool_start`. |
| M3 | Semaphore slots: `sem_slots`. |
| M4 | Completion reporting: `jq_commit` and the job board. |
| A1 | Priority scheduling with aging: `jq_effective_priority`. |
| A2 | Timeouts: leases, execution budgets, heartbeats. |
| A3 | Scaling: K live, drain, single-node mode. |
| A4 | Logging and monitoring: `logger.c`, `monitor.c`. |
| A5 | Statistics: `stats.c`, `experiments.c`, CSV. |

The final product is the dashboard, with Queued / Running / Completed / Failed columns.

**4. The course outline says socket-programming projects are not accepted. Why is this fine?**
The project is not *about* sockets. Every graded mechanism is threads and IPC primitives inside each
board. The boards are separate address spaces that exchange messages (message passing, OSC §3.4), and
the transport code is one small file (`proto.c`). The whole engine also runs on one board
(single-node mode), with no network at all.

**5. Why distribute jobs across several boards?**
- One board is a single failure domain: you cannot show one part failing while the rest
  continues, or detection and reassignment.
- The two cores of one board share memory and one kernel. Separate boards have private memory and
  must use messages, and that contrast is part of the lesson.
- Placement policies need at least two targets: with one worker, round-robin and least-loaded are identical.
- T5 compares the master alone with one, two, and three workers.

**6. Why a fractal? Is it an application?**
It is only a workload, about 20 lines of C (`kernels.c → run_fractal`):
- it is deterministic, so results can be checked with a CRC;
- tiles have uneven costs, so load balancing matters;
- every scheduling decision is visible: the centre (P3) paints first, and requeued tiles flash red.

---

## B. Threads and the pool

**7. Processes or threads?**
FreeRTOS tasks are threads: on each board they share one address space and are scheduled
independently. The four boards behave like four processes with separate memories that
communicate by messages.

**8. Why a fixed pool instead of one task per job?**
- Creating a task costs time and memory (its own stack). A fixed pool bounds memory and avoids creation cost per job (OSC §4.5.1).
- N = 4 executors are created at boot (`pool_start`) and reused forever.

**9. What is the difference between N and K?**
N = 4 is the number of executor tasks (fixed). K is the number of execution permits in the
counting semaphore `sem_slots`. An executor must take a permit before it takes a job, so at most K
jobs run. Executors without a permit stay BLOCKED on `sem_slots`.

**10. How do you change K while jobs are running?**
`pool_set_slots`:
- Raising K gives extra permits at once.
- Lowering K *retires* permits: a running job is never stopped, but when it returns its permit and there are more permits than wanted, that permit is not given back (`return_permit`). Idle permits are retired immediately.

**11. What happens when K exceeds the active core count?**
Each worker can execute two CPU-bound jobs in parallel. When K exceeds two, extra jobs share the
cores through time slicing. Test T4 measures the effect of raising K on the three workers.

**12. How does FreeRTOS schedule the executors?**
- Preemptive priority scheduling: each core runs the highest-priority ready task it is allowed to run.
- The four executors are distributed across the available cores at startup. ESP-IDF otherwise pins a task to the core on which it first uses floating-point registers.
- Tasks of equal priority are time-sliced on the 10 ms tick.
- This is the second level of scheduling; NEXUS only observes it.

---

## C. Synchronization

**13. Explain the job queue.**
It is the bounded-buffer solution (OSC §7.1.1):
- **Producer:** `take(sem_free)`, lock `q_mtx`, insert, unlock, `give(sem_ready)` (`jq_submit`).
- **Consumer:** `take(sem_ready)` (`jq_wait_ready`), then lock, remove the best job, unlock, `give(sem_free)` (`jq_pop_best`).
- Producers sleep when the buffer is full; consumers sleep when it is empty.

**14. If you have semaphores, why also a mutex?**
The semaphores only *count* free places and waiting jobs. The mutex makes the *update* of the array
and its counters atomic with respect to other tasks. Without it, two producers could write the same
slot at the same time.

**15. Why not just a FreeRTOS queue?**
A FreeRTOS queue is FIFO only, and we need priority order with aging. On the master the queue and
the in-flight table must also change together in one critical section, which a FreeRTOS queue cannot
do. We still use FreeRTOS queues where FIFO is right: `tx_queue` and `log_queue`.

**16. Mutex versus binary semaphore?**
- A mutex has an owner: only the task that took it should give it back.
- In FreeRTOS a mutex uses priority inheritance.
- A binary semaphore has no owner and no inheritance; it is for signalling.
- Lab D shows the difference.

**17. Show a race condition.** Lab A (`oslab.c → lab_a_task`)
- Two tasks, one on each core, each do `v = counter; v = v + 1; counter = v;` 100 000 times.
- Both can read the same old value, so one update overwrites the other. The final count is below 200 000.
- Lab B puts the same three lines inside a mutex and always gets 200 000, at the cost of locking time.

**18. Why use a spinlock (`taskENTER_CRITICAL`) in `exec_pool.c`?**
It protects a few counter updates of a few instructions. On the dual-core chip it disables
interrupts on this core and takes a spinlock, which also keeps out the other core. It must never be
held across anything that blocks; for longer sections we use mutexes.

**19. What is priority inversion? Explain lab D.**
A high-priority task H waits for a lock held by low task L, while a medium task M (which does not
need the lock) runs instead of L, so H effectively waits for M.

In lab D:
1. L takes the lock and works for 20 ms.
2. H blocks on the lock.
3. M runs for 300 ms.
4. With a binary semaphore, H waits for M too.
5. With a mutex, L inherits H's priority, M cannot preempt it, and H waits only for L (OSC §6.8.2, the Mars Pathfinder bug).

All three tasks are pinned to core 1. Otherwise core 0 could simply run L and the inversion would not appear.

**20. Can NEXUS deadlock?**
We prevent circular wait (OSC §8.3–8.5):
- `q_mtx`, `nodes_mtx`, the frame mutex and the stats mutex are never held two at a time (each module locks, copies, unlocks before calling another). The only nesting is the dashboard's build mutex, which is always the outermost lock.
- Blocking takes follow one order: `sem_free` or `sem_ready` first, then `sem_slots`, then `q_mtx`.
- The only semaphore taken while `q_mtx` is held (`sem_free`, when a job is requeued) uses timeout 0, so it never waits.
- With one fixed lock order there is no cycle, so no deadlock.

Lab E (extension) would show a deadlock caused by opposite lock order.

**21. What is starvation and how do you prevent it?**
Under strict priority, a steady stream of high-priority jobs can postpone low-priority jobs forever. Aging raises the effective priority by 1 for every 1.5 s of waiting, up to P3 (`jq_effective_priority`), so every job is eventually first. On the dashboard: set "Client producer tasks" to 4 (and lower K to 1 if needed, so the storm is bigger than the free slots), choose "Priority" and press "High-priority storm", and the P0 line climbs; switch to "Priority + aging" and it comes back down.

**22. Where else do you use producer–consumer?**
- `log_queue`: all tasks → logger, non-blocking.
- `tx_queue`: dispatcher/executors → link_tx, one per socket.
- Each worker's local job queue: link_rx → executors.

**23. What happens when the queue is full?**
Producers block on `sem_free` (client tasks, the frame producer, the experiment runner). The
dashboard's "producers blocked" counter counts client-task submits that had to wait. The supervisor never blocks: a job it cannot requeue right now
is marked `requeue_pending` and retried every 100 ms.

---

## D. Scheduling

**24. What are your two levels of scheduling?**
1. The master decides *which job* runs next and *on which board* (our code).
2. FreeRTOS on each board decides *which task* runs on *which core* (the kernel).

**25. Explain your ordering policies.**
- **FCFS:** lowest sequence number first.
- **Strict priority:** highest base priority (P0–P3), ties by sequence number.
- **Priority + aging:** effective priority = min(3, base + ⌊wait / 1.5 s⌋), ties by sequence number.

A requeued job keeps its sequence number and submit time, so it does not lose its turn.

**26. Explain your placement policies and credits.**
- A node is eligible if it is online, not draining, and has fewer than K + 1 jobs in flight (a credit).
- **Round-robin** takes the next eligible node in turn; **least-loaded** takes the lowest in-flight ÷ K.
- Why the +1: one job is prefetched so the worker does not sit idle during the network round trip.
- Why the limit: a worker never holds a large backlog, so a crash loses at most K + 1 jobs and recovery cannot dump everything on the other board.

**27. Why might FCFS and priority show the same *mean* latency?**
With equal-sized jobs and a scheduler that never idles, the order only redistributes waiting
between jobs. The difference appears in per-priority waits, maximum wait and p95, which is what T1 reports.

**28. How do you measure scheduling quality?**
- Per-priority mean and maximum queue wait (dispatch − submit).
- Mean, p95 and max latency (done − submit).
- Throughput and makespan per run (`stats.c → nx_stats_window`, `/api/runs.csv`).

---

## E. Message passing

**29. How do the boards talk?**
TCP over the master's own Wi-Fi network. Every message is a frame: an 8-byte header (`"NX"`,
version, type, length) and the payload. TCP is a byte stream, so `nx_recv_frame` reads exactly the
header and then exactly `length` bytes, however the network splits them.

**30. Why exactly one sender task per socket?**
If two tasks sent at the same time, the bytes of two frames could interleave and corrupt the
stream. Everything for a worker goes through its `tx_queue` to its `link_tx` task. That is a
single-writer design: no lock needed on the socket.

**31. How does a worker join, and how is it identified?**
1. The worker connects and sends HELLO with its MAC address, a random *incarnation* number chosen at boot, N and K.
2. The master matches the slot by MAC, never by IP (`cluster.c → admit`).
3. A changed incarnation means the board rebooted, so its old jobs are requeued.

**32. Why is Wi-Fi power saving off on the workers?**
The default modem-sleep mode wakes the radio only at beacon intervals (~100 ms), which would add
that delay to every DISPATCH (`wifi_sta.c → esp_wifi_set_ps(WIFI_PS_NONE)`).

---

## F. Fault tolerance

**33. How do you detect a failed worker?**
- Heartbeats (OSC §19.5.1): every worker sends one every 500 ms, carrying progress (running job ids, free slots, completed count) and its task table.
- The supervisor (every 100 ms) marks a worker SUSPECT after 1.5 s without any message, and OFFLINE after 3 s.
- We cannot rely on TCP or the access point: a board that loses power sends no FIN, and the access point only drops a silent station after 300 s.

**34. How fast is detection?**
- OFFLINE happens 3.0 to 3.1 s after the *last message* (one supervisor tick of jitter).
- Counted from the moment of the fault it is a bit shorter, because the last heartbeat came up to 0.5 s before the fault.
- For an injected fault (crash button, T3) we know the exact fault time and report the measured delay, whether the master notices through the timeout or because the rebooted board came back first with a new incarnation. For a physical unplug we report it from the last message.

**35. What happens to the failed worker's jobs?**
`go_offline → jq_requeue_node`: every in-flight job of that node goes back into the queue with
attempt + 1, keeping its sequence number and submit time, so it is near the front. A job that was
already on its 3rd attempt becomes FAILED instead. If the queue is full at that moment the job is
marked `requeue_pending` and the supervisor retries every 100 ms. The image shows
those tiles flashing red and then painted by the other worker.

**36. What is a lease, and why do you need it if you have heartbeats?**
- A lease is a per-job deadline on the master: execution budget + 2 s.
- Heartbeats detect a dead *board*. A lease catches a lost DISPATCH or RESULT, or a job stuck on a board that is still alive.
- On expiry the job is requeued (`jq_requeue_expired`).

**37. What is a runaway job and how do you stop it?**
A job that would run far longer than allowed. FreeRTOS cannot safely kill a task in the middle of
its work (it could hold a lock or memory), so every kernel checks the clock and cancels itself when
its budget is used up. The executor returns its permit and reports TIMEOUT; the master retries
(an ERROR result is treated the same way), and after 3 attempts the job is FAILED ("Submit
runaway job" button).

**38. Do you guarantee exactly-once execution?**
No.
- A job can execute twice, for example when a worker's result arrives after the master already requeued the job (heartbeat timeout or lease expiry) and it ran elsewhere.
- What we guarantee is **at-least-once execution** (while retries remain and the master is up) with **exactly-once commit**: the first valid result for a job id commits, later ones are counted as duplicates and dropped (`jq_commit`).
- The "Network glitch" button (4 s mute) produces this: Worker A is declared OFFLINE at 3 s and its
jobs are requeued; then its held results arrive and are committed or rejected as duplicates.

**39. What if a worker comes back?**
- Same incarnation (only the network was down): it may still deliver results, and a late result can still be the first valid one.
- New incarnation (it rebooted): whatever it was doing is gone, and its old jobs are requeued at once.

**40. What if both workers fail?**
Jobs stay queued; the dispatcher waits for a node with a credit, and producers block when the
queue fills. Turning on single-node mode lets the master's own pool continue.

**41. What if the master fails?**
The master is a single point of failure, which is a stated limitation. Surviving it would need a
replicated queue and coordinator election (OSC §19.5), which is out of scope.

**42. Is unplugging a worker "tamper detection"?**
No. It demonstrates crash-failure detection through missing heartbeats. A missing heartbeat cannot
tell a crash from a cut cable or a tampering attempt.

---

## G. FreeRTOS on the ESP32-S3

**43. Why are network tasks at a higher priority than executors?**
If executors (priority 5) could starve `link_tx` (10), a busy worker would stop sending heartbeats
and be declared dead while it is working. All our tasks stay below ESP-IDF's TCP/IP (18) and Wi-Fi
(23) tasks for the same reason.

**44. What is the task watchdog and why the `vTaskDelay(1)`?**
- ESP-IDF watches the two idle tasks. If an idle task cannot run for 5 s, it prints a warning.
- Two CPU-bound executors working back-to-back could keep both cores busy forever, so each executor sleeps one tick at least once per second (`executor_task`).

**45. Why `esp_timer_get_time()` and not the tick count?**
The tick is 10 ms (100 Hz), too coarse for job times. `esp_timer` counts microseconds.

**46. How do you compute CPU %?**
- `uxTaskGetSystemState` gives each task's run-time counter.
- The CPU share is the difference between two samples divided by the elapsed time × 2 cores.
- Per-core load is 100 − the share of that core's idle task (`monitor.c`).

**47. Is your task table a trace of the scheduler?**
No. It is sampled twice a second; a task may change state many times between samples. The
dashboard says so. The "usually waits on" column is from our design, not read from the kernel.

**48. Where does memory go?**
- Task stacks are in internal RAM.
- Large tables live in the 8 MB PSRAM, allocated once at boot (no allocation per job): the job queue (256 entries), the 600×400 image, the statistics ring (4096 jobs), and the executors' scratch buffers.
- Small tables (the 32-entry in-flight table, the workers' 8-place local queues) stay in internal RAM: `alloc_table` uses PSRAM only above 4 KB.

---

## H. Measurement and results

**49. Which clock measures latency?**
Only the master's. Submit, dispatch and done times are all taken on the master, so the boards'
clocks never need to agree. Workers report their execution time as a duration.

**50. How do you make experiments fair?**
For each run, the experiment runner (`experiments.c`):
- waits until the cluster is idle;
- pauses client tasks and auto-zoom;
- applies one configuration;
- submits a fixed, seeded workload;
- tags every commit with the run id.

While a suite runs, the master refuses every other dashboard control. In T1, T4 and T5 every configuration runs three times; T2 and T3 run a single-node reference and then the test once.

**51. How do you prove correctness?**
- T2 renders the same fractal view on a single board (reference) and then on two workers with four producer tasks running. The image CRC32 must be identical, every job committed, none failed.
- T3 requests a Worker A restart at the 40% progress threshold. It requires a confirmed new boot incarnation for A, requeued work completed elsewhere, and the same final image CRC.
- Lab C and the `peak` gauge show the semaphore limit holds.

**52. What limits the scaling from one worker to three (T5)?**
Gains depend on job size (Amdahl's law, OSC §4.2):
- Larger jobs give each worker more computation per network exchange, which can improve scaling.
- For tiny jobs, the master's per-job work and the radio become the serial part and the gain shrinks.

We report what we measure, including no gain.

---

## I. "Show me where …"

| Concept | Open |
|---|---|
| Bounded buffer, producer | `job_queue.c → jq_submit` |
| Bounded buffer, consumer | `job_queue.c → jq_wait_ready`, `jq_pop_best` |
| Aging | `job_queue.c → jq_effective_priority` |
| Thread pool | `exec_pool.c → pool_start`, `executor_task` |
| Counting semaphore (K) | `exec_pool.c → sem_slots`, `pool_set_slots`, `return_permit` |
| Timeout by cooperative cancel | `kernels.c → run_fractal` (deadline check) |
| Placement and credits | `cluster.c → choose_node` |
| Heartbeat failure detection | `cluster.c → supervisor_task`, `go_offline` |
| Exactly-once commit | `job_queue.c → jq_commit` |
| Message framing | `proto.c → nx_recv_frame` |
| Single sender per socket | `cluster.c → link_tx_task`, `worker_link.c → link_tx_task` |
| Logging thread | `logger.c → nx_log`, `logger_task` |
| Task monitor | `monitor.c → monitor_sample` |
| Race / mutex / semaphore / inversion | `oslab.c` labs A–D |
| Experiments | `experiments.c → run_suite`, `do_run` |

---

## J. 10-minute demo script

| Time | Do | Say |
|---|---|---|
| 0:00 | Open http://192.168.4.1/ on the laptop | Four boards, local Wi-Fi, two levels of scheduling |
| 1:00 | Point at the job board, the fractal and the queue bar | Every tile is a job; bounded buffer with a mutex and two semaphores |
| 2:00 | Set client tasks to 4, choose **Priority**, press **High-priority storm** | P0 line climbs: starvation. Switch to **Priority + aging**: the line comes back down |
| 3:30 | Worker A slots: press **+** up to 4 | K permits vs N tasks. Above K = 2, CPU-bound jobs time-slice on the worker's two cores |
| 4:30 | Lab **A**, **B**, **D** | Lost updates; the mutex fixes them; priority inheritance shortens H's wait |
| 6:00 | **Unplug Worker A** during a frame | Failed link detected, tiles requeued to B and C, detection and recovery shown on the dashboard |
| 7:30 | Plug Worker A back | It rejoins with a new incarnation |
| 8:00 | Show the last experiment results and `/api/runs.csv` | Graphs in the report come from these CSV files |
| 9:00 | Open `job_queue.c` (`jq_submit`, `jq_pop_best`), then `exec_pool.c` (`executor_task`, `pool_set_slots`) | M1 in the queue, M2–M3 in the pool |

**If Wi-Fi misbehaves:** switch on **Single-node mode**. The whole engine runs on the master, so
every OS concept can still be shown.

---

## K. Honest limitations (say them before you are asked)

- The master is a single point of failure.
- Execution is at-least-once; only the commit is exactly-once.
- The task table is sampled, not a trace.
- Speed-up depends on job size; we report measured values.
- Plain HTTP on a password-protected lab network; fine for a demo, not for production.
- The fractal is a workload, not an application; there is no security or domain logic.
