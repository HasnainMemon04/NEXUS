# NEXUS firmware

NEXUS is a C11 and FreeRTOS task orchestration engine for FAST-NUCES CS-2006 Project #2. An ESP32-S3 master coordinates three workers over its own Wi-Fi. The embedded Control Center shows Mandelbrot tiles as the scheduler assigns and recovers jobs.

## Match the firmware to the board

| Role | Board | Active FreeRTOS cores | ESP-IDF target | Build directory |
| --- | --- | ---: | --- | --- |
| Master | ESP32-S3-N16R8, 16 MB flash, 8 MB octal PSRAM | 2 | `esp32s3` | `build/master-esp32s3` |
| Worker A | First ESP32 DevKit, 4 MB profile | 2 | `esp32` | `build/worker-esp32` |
| Worker B | Second ESP32 DevKit, 4 MB profile | 2 | `esp32` | `build/worker-esp32` |
| Worker C | ESP32-WROOM-32U, 4 MB profile | 2 | `esp32` | `build/worker-esp32` |
| Optional worker | ESP32-S3-N16R8 | 2 | `esp32s3` | `build/worker-esp32s3` |

Flash all three classic workers with the same current image so each can accept any worker slot. Each worker starts with two execution permits and four executor tasks. The dashboard can raise K to four to demonstrate time slicing. Connect a compatible 2.4 GHz antenna to the powered-off WROOM-32U before first use. A bare module also needs a regulated 3.3 V carrier with USB-to-UART and boot/reset circuitry. Follow your board pinout and the [Espressif module datasheet](https://documentation.espressif.com/esp32-wroom-32d_esp32-wroom-32u_datasheet_en.html).

## Build and flash

Activate ESP-IDF 6.1 in PowerShell and run these commands from the firmware directory. Replace the example COM ports with the ports in Device Manager.

```powershell
. C:/esp/esp-idf/export.ps1
$env:PYTHONUTF8 = '1'

Push-Location master
idf.py -B ../build/master-esp32s3 -DIDF_TARGET=esp32s3 build
idf.py -B ../build/master-esp32s3 -DIDF_TARGET=esp32s3 -p COM5 flash monitor
Pop-Location

Push-Location worker
idf.py -B ../build/worker-esp32 -DIDF_TARGET=esp32 build
idf.py -B ../build/worker-esp32 -DIDF_TARGET=esp32 -p COM6 flash
idf.py -B ../build/worker-esp32 -DIDF_TARGET=esp32 -p COM7 flash
idf.py -B ../build/worker-esp32 -DIDF_TARGET=esp32 -p COM8 flash
Pop-Location
```

Check the port each time you swap boards. Two USB-to-UART adapters may reuse the same COM number. Keep the explicit target and build path on every command. An existing classic worker build should have `CONFIG_FREERTOS_UNICORE` disabled so both cores run. Existing configurations retain previously saved values. Run `idf.py -B ../build/worker-esp32s3 -DIDF_TARGET=esp32s3 build` from `worker/` for the optional S3 image. For custom Wi-Fi settings, run `menuconfig` with the same role and target arguments, open **NEXUS**, and rebuild every participating role with matching credentials.

To identify the current board before flashing, run `python -m esptool --port COM5 --no-stub read-mac` with the detected port. Esptool reports its chip type, hardware features, and MAC address.

### Flash the merged images

The merged images contain the bootloader, partition table, and application. From `firmware/` with the ESP-IDF environment active, flash the images produced under `release/` at offset `0x0`.

```powershell
python -m esptool --chip esp32s3 --port COM5 write-flash 0x0 release/nexus-master-esp32s3-flash.bin
python -m esptool --chip esp32 --port COM6 write-flash 0x0 release/nexus-worker-esp32-flash.bin
python -m esptool --chip esp32 --port COM7 write-flash 0x0 release/nexus-worker-esp32-flash.bin
python -m esptool --chip esp32 --port COM8 write-flash 0x0 release/nexus-worker-esp32-flash.bin
```

Run `./tools/package.ps1` after building the master and classic worker to regenerate the images and their SHA-256 manifest. Build the optional S3 worker and add `-IncludeS3Worker` to package it as well.

## Start the cluster

1. Power the S3 master with all workers off. After the access point starts, power the first DevKit and wait for Worker A. Power the second DevKit and wait for Worker B. Finally, power the WROOM-32U and wait for Worker C. The master assigns slots in join order and tracks each board by MAC across reconnects until the master restarts.
2. Connect the laptop or phone to **NEXUS-OS** using the configured WPA2 password. The source default is `nexus-os-lab`. Keep the device connected if it warns that the network has no internet.
3. Open [the Control Center](http://192.168.4.1/). Verify that all three workers show **ESP32**, two active cores, online status, two acknowledged execution slots, and recent heartbeats. The master also reports two active cores. Each worker uses DHCP, so check its current IP on the dashboard.
4. Click the fractal to request a fresh frame. Check that A, B, and C all complete work. Enable **Single-node mode** and request another frame to route new jobs to the master. Work already dispatched to remote nodes may finish there.

## Project structure

- `master/` contains the access point, scheduler, failure supervisor, experiment runner, and gzip-embedded dashboard.
- `worker/` contains the station and job transport. Target-specific defaults select classic internal RAM or S3 octal PSRAM.
- `components/nexus_core/` contains the shared queue, executor pool, workloads, protocol, monitor, logger, and labs.
- `tests/` contains automated checks. The firmware directory can be published with its defaults, partition files, documentation, and tests. Ignore generated configurations, build directories, and release output.

From the repository root, run `./firmware/tests/host/run.ps1` for native queue and protocol tests. With Espressif QEMU and ESP-IDF active, run `./firmware/tests/device/run.ps1` for both target-specific device self-tests. See the [browser smoke test](tests/browser/README.md) for embedded dashboard validation.

See the [proposal checklist and hardware procedure](docs/PROPOSAL_CHECKLIST.md), [code tour](docs/CODE_TOUR.md), and [viva guide](docs/VIVA_GUIDE.md).
