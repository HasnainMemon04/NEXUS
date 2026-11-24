/*
 * cluster.h — the master's view of the cluster: nodes, links, dispatcher and
 * supervisor. Node 0 is the master's own engine (single-node mode); nodes 1–3
 * are the worker boards, identified by MAC address, never by IP.
 */
#pragma once
#include "job_queue.h"
#include "proto.h"
#include "placement.h"

typedef enum { NODE_EMPTY = 0, NODE_ONLINE, NODE_SUSPECT, NODE_OFFLINE } nx_node_state_t;

typedef struct {
    char     name[10];
    uint8_t  state;          /* nx_node_state_t                         */
    uint8_t  mac[6];
    uint32_t inc;            /* incarnation reported in HELLO            */
    uint32_t ip;             /* network byte order                       */
    uint8_t  k, n;           /* acknowledged execution permits / pool size */
    uint8_t  requested_k, chip, cores;
    uint16_t cpu_mhz;
    bool     slots_ready, slot_ack_supported;
    bool     drain;          /* no new jobs, running ones finish         */
    int64_t  last_heard, offline_at, fault_at;
    uint32_t dispatched, results, duplicates;
    uint32_t failures, failure_ticket, failure_detect_ms;
    uint32_t failure_requeued, failure_recovered, recovered_elsewhere;
    int64_t  failure_at_us, failure_recovery_at_us;
    bool     hb_valid;
    nx_heartbeat_t hb;       /* last progress message (task table, load) */
} nx_node_view_t;

void    cluster_start(nx_job_queue_t *gq);
void    cluster_view(nx_node_view_t out[NX_NODES]);
void    cluster_set_placement(uint8_t placement);
uint8_t cluster_placement(void);
bool    cluster_set_slots(uint8_t node, uint8_t k);
void    cluster_set_drain(uint8_t node, bool on);
void    cluster_set_worker_drain(bool a, bool b, bool c);    /* publish both routing changes together */
void    cluster_set_local(bool on);
bool    cluster_local(void);
bool    cluster_mute(uint8_t node, uint32_t ms);       /* simulated network fault  */
uint32_t cluster_reboot(uint8_t node);                 /* crash request ticket or zero */
bool    cluster_run_lab(uint8_t node, uint8_t lab);    /* sync lab on that board   */
