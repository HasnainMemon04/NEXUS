# NEXUS v2.1 implementation and verification checklist

The submitted proposal describes a three-node ESP32-S3 design. The final deployment uses an ESP32-S3-N16R8 master, two ESP32 DevKits, and one ESP32-WROOM-32U. All four boards use two active FreeRTOS cores each. The local access point, Mandelbrot workload, and proposal objectives apply to the four-board cluster.

| Objective | Course feature | Implementation | Physical acceptance evidence |
| --- | --- | --- | --- |
| O1 | M1 shared queue | Bounded producer-consumer queue with a mutex, free and ready counts, and per-board execution queues | T2 records four participating producers, all expected jobs, and unique commits |
| O2 | M2 fixed pool and M3 semaphore | Four executors per board and one to four live permits | T4 confirms K on all three workers, measures throughput and latency, and observes running count |
| O3 | M4 completion reporting | Job lifecycle, one successful commit per ID, checked tile payloads, and per-job checksums in CSV | T2 matches master-only image CRC and verifies unique commits |
| O4 | A1 priority scheduling | FCFS, strict priority, aging, round-robin, and capacity-aware least-loaded placement | T1 compares all six combinations on the same staged backlog |
| O5 | A2 timeouts and recovery | Job budget, lease, heartbeat state, reconnect identity, bounded retry, and requeue | T3 confirms Worker A reboot and reassignment to Worker B. Also unplug A and submit a runaway job |
| O6 | A3 dynamic scaling | Live permits, correlated worker acknowledgment, drain, and master-only routing | T4 confirms every slot setting. T5 compares worker participation and speedup |
| O7 | A4 logging and monitoring, A5 statistics | Nonblocking logger, sampled task and CPU counters, queue timings, percentiles, and CSV | Capture heap and stack at idle and peak, and export both CSV files |
| O8 | Synchronization lab | Labs A–D exercise lost updates, mutexes, counting semaphores, and priority inheritance | Run all four labs on every board and record counts and timing |

## Four-board acceptance procedure

1. Flash the matching targets and record firmware hashes, board types, MACs, and 115200-baud serial output. Power the master, then the first DevKit (A), second DevKit (B), and WROOM-32U (C). Wait for each board to join before powering the next. Confirm that all three workers receive tiles and report two active cores each. Enable master-only mode and verify that new dispatches go to the master. Turn the mode off and confirm that distributed scheduling resumes.
2. Run T1 through T5 from the dashboard. Save the Runs CSV and Jobs CSV after each suite because the job history is bounded. The runner pauses background producers and auto-zoom and uses fixed inputs. Keep skipped and failed runs in the record.
3. For **T1**, graph per-priority mean and maximum wait for three repeats of all six policy combinations. Include p95 latency and throughput for context. For **T2**, require four producers, 310 distinct commits, both workers represented, zero failed jobs, and matching reference CRC.
4. For **T3**, require positive requeues and work recovered on Worker B, then check that Worker A returns under the same MAC with a new boot incarnation. Record the trigger tile count, detection delay, and recovery time. Outside the suite, unplug one worker during a render and check that the other two finish its work. Reconnect it and verify that its role returns. Submit a runaway job and check budget cancellation, the three-attempt limit, and released permits.
5. For **T4**, verify each confirmed K from one to four on A, B, and C, then graph throughput and latency. Observe concurrency while the slot count is stable. For **T5**, divide the master-only makespan by the compared makespan for one, two, or three workers with the same matrix size and job count. All participating nodes use K = 2. Plot both sizes. The Runs CSV includes `by_c` to verify Worker C's commits.
6. For the labs, A should lose updates, B should reach 200,000, C should finish 60 jobs with at most two permits, and D should show a shorter high-priority wait with a mutex. Repeat the labs and worker reconnects while recording free heap, stack high-water marks, and any watchdog or allocation errors.

## Evidence levels

USB chip reads confirmed two main CPU cores on each board. The master reported an ESP32-S3 with 8 MB PSRAM. All three classic workers reported ESP32-D0WD-V3. Compile and emulator checks cover the selected targets and exercised OS mechanisms. Browser fixtures use synthetic telemetry. The four-board build still needs a physical session to measure Wi-Fi association, reconnect behavior, mixed-board benchmark results, and heap and stack usage under radio load.

### Automated results

- **Native tests passed.** The host suite exercised FCFS, strict priority, aging, bounded queue accounting, retry and lease handling, duplicate rejection, master-only routing, three-worker capacity-aware placement, and fragmented, interrupted, coalesced, and invalid TCP frames.
- **Browser smoke tests passed at 1440×900 and 390×844.** The embedded HTML received fixture telemetry and tiles. Checks covered Worker C status, tile ownership, slot, drain, mute, reboot, lab controls, benchmark job counts, both one-core and two-core telemetry, keyboard zoom, and browser reconnect feedback.
- **ESP32 QEMU passed the worker-independent FreeRTOS self-test with two active cores.** Both chip profiles produced the same overview frame CRC `89c14f24`, detail CRC `4208557d`, and seeded 32×32 matrix CRC `af160840`. All four permit limits and the execution timeout passed. Lab A recorded 106,795 of 200,000 increments, Lab B recorded all 200,000, Lab C completed 60 jobs at a peak of two, and Lab D measured a 328,223 µs binary-semaphore wait and a 22,327 µs mutex wait.
- **ESP32 emulated heap and stack.** Internal free heap was 250,328 bytes before and after the repeated render, with a recorded minimum of 233,976 bytes. The smallest executor stack high-water mark was 2,208 bytes. The self-test uses the production kernels and pool without a Wi-Fi or dashboard stack.
- **ESP32-S3 QEMU passed with two active cores.** It passed all permit, timeout, and lab checks. Lab B reached 200,000 and Lab C finished 60 jobs with a peak of two. Internal free heap was 343,836 bytes before and after the repeated render, with a recorded minimum of 327,360 bytes. The smallest executor stack high-water mark was 1,984 bytes.

Reproduce these checks with the scripts in [tests](../tests). The QEMU results cover the shared queue, executor pool, kernels, and synchronization labs. Use T1–T5 on the four physical boards for Wi-Fi, speedup, and latency measurements.

### Flash and memory

| Profile | Application image | Static DRAM data + BSS |
| --- | ---: | ---: |
| ESP32-S3 master | 898,192 bytes | 85,772 bytes |
| ESP32 worker | 802,384 bytes | 61,375 bytes |
| Optional ESP32-S3 worker | 756,016 bytes | 62,572 bytes |

These are application and linker measurements. Each executor's kernel scratch is 2,176 bytes. The embedded dashboard is 21,959 bytes gzipped, with its contents verified byte for byte against the HTML source. The master uses a 12,840-byte tile response buffer and serves subsequent pages using a watermark. When browsers are connected, WebSocket telemetry is limited to four updates per second during jobs and two per second while idle.

The merged images were checked against their SHA-256 manifest, chip headers, boot and app offsets, partition tables, and application bytes. Record the four-board Wi-Fi and T1–T5 results after flashing.
