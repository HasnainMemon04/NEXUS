/*
 * proto.h — messages between master and workers.
 *
 * OS CONCEPT: message passing between separate address spaces (OSC §3.4, §3.6).
 * TCP delivers a byte stream, so every message is a frame: an 8-byte header
 * with its payload length, then the payload. The receiver reads exactly that
 * many bytes, however the network splits them. Each socket has exactly one
 * sending task, so frames can never interleave.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "nexus_config.h"
#include "nexus_types.h"
#include "monitor.h"
#include "oslab.h"

#define NX_MAGIC    0x584E   /* "NX" */
#define NX_VERSION  1

enum {
    NX_MSG_HELLO = 1,    /* worker -> master: who I am                    */
    NX_MSG_WELCOME,      /* master -> worker: your node number            */
    NX_MSG_DISPATCH,     /* master -> worker: run this job                */
    NX_MSG_ACK,          /* worker -> master: job accepted                */
    NX_MSG_RESULT,       /* worker -> master: result (+ tile pixels)      */
    NX_MSG_HEARTBEAT,    /* worker -> master: progress + task table       */
    NX_MSG_CONTROL,      /* master -> worker: change K, inject a fault ... */
};

enum {
    NX_CTL_SET_SLOTS = 1,      /* arg = new K                                   */
    NX_CTL_MUTE_LINK = 2,      /* arg = ms: stop sending (simulated link fault) */
    NX_CTL_REBOOT = 3,         /* restart the board (simulated crash)           */
    NX_CTL_RUN_LAB = 4,        /* arg = lab number                              */
};

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  version;
    uint8_t  type;
    uint16_t len;        /* payload bytes */
    uint16_t reserved;
} nx_frame_hdr_t;

typedef struct __attribute__((packed)) {
    uint8_t  mac[6];
    uint32_t incarnation;  /* random, chosen at every boot */
    uint8_t  pool_n, slots_k;
} nx_hello_t;

typedef struct __attribute__((packed)) { uint8_t node; } nx_welcome_t;
typedef struct __attribute__((packed)) { uint32_t id; uint16_t attempt; } nx_ack_t;
typedef struct __attribute__((packed)) { uint8_t cmd; uint32_t arg; } nx_control_t;

typedef struct __attribute__((packed)) {
    uint32_t seq, uptime_ms, heap_free, completed, timeouts;
    uint8_t  k, n, running, peak_running;
    uint8_t  core_load[2];
    uint8_t  n_running_ids, ntasks;
    uint32_t running_ids[NX_POOL_SIZE];
    nx_lab_results_t lab;            /* last sync-lab measurements on this board */
    nx_task_info_t tasks[NX_MAX_TASKS];
} nx_heartbeat_t;

#define NX_HEARTBEAT_LEN(ntasks) ((uint16_t)(sizeof(nx_heartbeat_t) - sizeof(nx_task_info_t) * (NX_MAX_TASKS - (ntasks))))

/* Optional trailer after the populated task rows. A v1 peer ignores extra
 * payload bytes, and SET_SLOTS keeps the permit count in its original byte. */
#define NX_HB_EXT_VERSION 1
#define NX_SLOT_CONTROL_GENERATION(arg) ((uint32_t)(arg) >> 8)

enum { NX_CHIP_UNKNOWN = 0, NX_CHIP_ESP32, NX_CHIP_ESP32S3 };

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t chip;
    uint8_t cores;
    uint8_t slots_effective;  /* permits still in circulation during a downscale */
    uint16_t cpu_mhz;
    uint32_t slot_ack;        /* SET_SLOTS generation applied on this connection */
} nx_heartbeat_ext_t;

_Static_assert(sizeof(nx_frame_hdr_t) == 8, "v1 frame header layout");
_Static_assert(sizeof(nx_hello_t) == 12, "v1 hello layout");
_Static_assert(sizeof(nx_control_t) == 5, "v1 control layout");
_Static_assert(sizeof(nx_job_spec_t) == 32, "v1 job layout");
_Static_assert(sizeof(nx_result_t) == 18, "v1 result layout");
_Static_assert(sizeof(nx_heartbeat_ext_t) == 10, "heartbeat trailer layout");


/* Send header + up to two payload parts in one frame. Returns false with errno on error. */
bool nx_send_frame(int sock, uint8_t type, const void *a, uint16_t alen, const void *b, uint16_t blen);
/* On receive failure, errno is zero for EOF, EBADMSG for an incomplete or invalid
 * frame, or the socket error. A partial frame requires closing the stream. */
int  nx_recv_frame(int sock, uint8_t *buf, uint16_t cap, uint8_t *type);
