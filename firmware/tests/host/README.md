# Host queue and protocol tests

Run with PowerShell and a native GCC toolchain on PATH.

~~~powershell
./firmware/tests/host/run.ps1
~~~

The tests compile the production queue and framing modules as native code. Minimal
FreeRTOS and lwIP stubs force deterministic queue capacity and TCP framing
conditions. This covers ordering, semaphore balances, timeout and worker retry,
master-only routing, capacity and credits, assignment validation, duplicate results, saturated requeue progress, fragmented
or coalesced frames, interruption, and invalid frame rejection. Run the ESP-IDF
self-test for actual task and semaphore behavior, and the three-board plan for
real Wi-Fi and hardware performance evidence.
