/*
 * cluster.c — scheduling across boards and failure handling on the master.
 *
 * Tasks (priorities from nexus_config.h):
 *   listener      accepts worker connections, reads HELLO, assigns a node slot
 *   link_rx_A/B/C one receiver per worker: ACK, RESULT, HEARTBEAT
 *   link_tx_A/B/C the ONLY sender on each worker socket (fed by tx_queue)
 *   dispatcher    consumer of the global job queue: ordering x placement
 *   supervisor    heartbeat timeouts, lease expiry, pending requeues
 *   monitor       samples the master's own FreeRTOS tasks twice a second
 *
 * Locking rule (deadlock prevention, OSC §8.5): nodes_mtx and the queue's
 * q_mtx are never held at the same time. The routing mutex serializes dispatch,
 * control changes and marking a worker offline. Per-node I/O locks protect
 * socket lifetime across a send and close; when both are needed, take routing
 * before I/O before nodes_mtx. No socket write holds nodes_mtx.
 */
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_rom_crc.h"
#include "exec_pool.h"
#include "logger.h"
#include "monitor.h"
#include "oslab.h"
#include "stats.h"
#include "producers.h"
#include "cluster.h"

typedef struct {
    uint32_t epoch, fault_ticket;
    uint8_t  type;
    uint16_t len;
    uint8_t  payload[sizeof(nx_job_spec_t) + 8];
} link_msg_t;

typedef struct {
    nx_node_view_t v;
    int            sock;        /* owned and closed by link_rx, -1 if idle */
    uint32_t       epoch;       /* one lifetime of a connection in this slot */
    uint32_t       slot_generation, sent_slot_generation, active_fault_ticket;
    bool           reboot_pending;
    uint32_t       sent_hb_seq;
    SemaphoreHandle_t io_mtx;  /* no descriptor closes during a send or shutdown */
    QueueHandle_t  txq;         /* tx_queue for link_tx */
    TaskHandle_t   rx_task;
} node_t;

static node_t            s_nodes[NX_NODES];
static SemaphoreHandle_t s_nodes_mtx, s_route_mtx;
static nx_job_queue_t   *s_gq;                 /* global job queue */
static nx_job_queue_t    s_local_q;            /* master's own engine */
static nx_pool_t         s_local_pool;
static bool              s_local_on;
static uint8_t           s_placement = NX_PLACE_LEAST_LOADED;
static uint32_t          s_fault_sequence;
static uint8_t           s_rr;

#define NODES_LOCK()   xSemaphoreTake(s_nodes_mtx, portMAX_DELAY)
#define NODES_UNLOCK() xSemaphoreGive(s_nodes_mtx)
#define ROUTE_LOCK()   xSemaphoreTake(s_route_mtx, portMAX_DELAY)
#define ROUTE_UNLOCK() xSemaphoreGive(s_route_mtx)

/* ------------------------------ results ------------------------------ */

static void on_result(uint8_t node, const nx_result_t *res, const uint8_t *data)
{
    if (res->status != NX_RES_OK) {             /* TIMEOUT (budget used up) or ERROR */
        if (jq_requeue_one(s_gq, res->id, res->attempt, node))
            nx_log("MST", NX_LOG_WARN, "job #%lu %s on %s: requeued (or FAILED after %d attempts)",
                   (unsigned long)res->id, res->status == NX_RES_TIMEOUT ? "exceeded its budget" : "failed",
                   s_nodes[node].v.name, NX_MAX_ATTEMPTS);
        return;
    }
    if (res->data_len > 0 && esp_rom_crc32_le(0, data, res->data_len) != res->checksum) {
        (void)jq_requeue_one(s_gq, res->id, res->attempt, node);
        nx_log("MST", NX_LOG_WARN, "job #%lu checksum mismatch", (unsigned long)res->id);
        return;
    }

    nx_job_t job;
    nx_commit_t commit = jq_commit(s_gq, res, node, &job);
    if (commit == NX_COMMIT_OK) {
        /* Publish tile and recovery evidence before the experiment sees this commit. */
        if (job.spec.type == NX_JOB_FRACTAL) frame_on_tile(&job.spec, data, node);
        NODES_LOCK();
        s_nodes[node].v.results++;
        if (job.recovery_from < NX_NODES && job.recovery_from != node &&
            job.spec.attempt >= job.recovery_min_attempt) {
            nx_node_view_t *failed = &s_nodes[job.recovery_from].v;
            failed->recovered_elsewhere++;
            if (failed->failures == job.recovery_serial) {
                failed->failure_recovered++;
                failed->failure_recovery_at_us = job.t_done;
            }
        }
        NODES_UNLOCK();
        nx_stats_on_commit(&job, res);
    } else if (commit == NX_COMMIT_DUPLICATE) {
        nx_stats_on_duplicate();
        NODES_LOCK();
        s_nodes[node].v.duplicates++;
        NODES_UNLOCK();
        nx_log("MST", NX_LOG_WARN, "duplicate RESULT for job #%lu from %s rejected",
               (unsigned long)res->id, s_nodes[node].v.name);
    } else {
        (void)jq_requeue_one(s_gq, res->id, res->attempt, node);
        nx_log("MST", NX_LOG_WARN, "invalid RESULT for job #%lu from %s rejected",
               (unsigned long)res->id, s_nodes[node].v.name);
    }
}

static void local_result(const nx_job_t *job, const nx_result_t *res, const uint8_t *data, void *arg)
{
    on_result(NX_NODE_LOCAL, res, data);
}

/* ------------------------------ failures ------------------------------ */

/* A nonzero silent_ms requests a fresh timeout check under nodes_mtx. */
static void go_offline(uint8_t slot, uint32_t epoch, uint32_t silent_ms, const char *why)
{
    node_t *nd = &s_nodes[slot];
    ROUTE_LOCK();
    xSemaphoreTake(nd->io_mtx, portMAX_DELAY);
    NODES_LOCK();
    int64_t now = esp_timer_get_time();
    int64_t heard = nd->v.last_heard;
    if (nd->epoch != epoch || nd->v.state == NODE_OFFLINE || nd->v.state == NODE_EMPTY ||
        (silent_ms && now - heard < (int64_t)silent_ms * 1000)) {
        NODES_UNLOCK();
        xSemaphoreGive(nd->io_mtx);
        ROUTE_UNLOCK();
        return;
    }
    int64_t fault = nd->v.fault_at;
    nd->v.state = NODE_OFFLINE;
    nd->v.offline_at = now;
    nd->v.slots_ready = false;
    nd->reboot_pending = false;
    nd->v.fault_at = 0;
    nd->v.failure_at_us = now;
    nd->v.failure_recovery_at_us = 0;
    nd->v.failure_detect_ms = (uint32_t)((now - (fault ? fault : heard)) / 1000);
    nd->v.failure_ticket = nd->active_fault_ticket;
    nd->v.failure_requeued = 0;
    nd->v.failure_recovered = 0;
    uint32_t failure = ++nd->v.failures;
    uint32_t delay = nd->v.failure_detect_ms;
    if (nd->sock >= 0) shutdown(nd->sock, SHUT_RDWR);
    NODES_UNLOCK();
    xSemaphoreGive(nd->io_mtx);

    int n = jq_requeue_node(s_gq, slot, failure);
    NODES_LOCK();
    nd->v.failure_requeued = n;
    NODES_UNLOCK();
    ROUTE_UNLOCK();
    nx_stats_on_failure(slot, delay, n, fault != 0);
    nx_log("SUP", NX_LOG_ERR, "%s OFFLINE (%s) after %lu ms: %d in-flight jobs for retry",
           nd->v.name, why, (unsigned long)delay, n);
}

/* Validate a v1 heartbeat before exposing its task-table count to web callers. */
static bool decode_heartbeat(const uint8_t *buf, int len, nx_heartbeat_t *hb,
                             nx_heartbeat_ext_t *ext)
{
    const size_t fixed = offsetof(nx_heartbeat_t, tasks);
    if (len < (int)fixed) return false;
    memset(hb, 0, sizeof(*hb));
    memcpy(hb, buf, fixed);
    if (hb->ntasks > NX_MAX_TASKS || hb->n_running_ids > NX_POOL_SIZE ||
        hb->n == 0 || hb->n > NX_POOL_SIZE || hb->k == 0 || hb->k > hb->n ||
        hb->running > hb->n || hb->peak_running > hb->n)
        return false;
    size_t base = NX_HEARTBEAT_LEN(hb->ntasks);
    if ((size_t)len != base && (size_t)len != base + sizeof(*ext)) return false;
    memcpy(hb->tasks, buf + fixed, base - fixed);
    memset(ext, 0, sizeof(*ext));
    if ((size_t)len > base) memcpy(ext, buf + base, sizeof(*ext));
    for (int i = 0; i < hb->ntasks; i++)
        hb->tasks[i].name[sizeof(hb->tasks[i].name) - 1] = '\0';
    return true;
}

static void apply_heartbeat_locked(node_t *nd, const nx_heartbeat_t *hb,
                                   const nx_heartbeat_ext_t *ext)
{
    nd->v.hb = *hb;
    nd->v.hb_valid = true;
    nd->v.n = hb->n;
    if (ext->version == NX_HB_EXT_VERSION && ext->cores > 0 && ext->cores <= 2 &&
        ext->slots_effective <= hb->n && ext->slots_effective != 0) {
        nd->v.chip = ext->chip;
        nd->v.cores = ext->cores;
        nd->v.cpu_mhz = ext->cpu_mhz;
        nd->v.slot_ack_supported = true;
        nd->v.k = ext->slots_effective;
        nd->v.slots_ready = ext->slot_ack == nd->slot_generation &&
            nd->v.k == nd->v.requested_k && hb->k == nd->v.requested_k;
        return;
    }

    /* Older v1 workers still dispatch normally. Their heartbeat offers a
     * best-effort UI indication but has no correlated acknowledgment. */
    if (nd->v.slot_ack_supported) {
        nd->v.slots_ready = false;
    } else {
        nd->v.k = hb->k;
        nd->v.slots_ready = nd->slot_generation == 0 ||
            (nd->sent_slot_generation == nd->slot_generation && hb->seq != nd->sent_hb_seq &&
             hb->k == nd->v.requested_k);
    }
}

static bool handle_frame(uint8_t slot, uint32_t epoch, uint8_t type, const uint8_t *buf, int n)
{
    nx_heartbeat_t hb;
    nx_heartbeat_ext_t ext;
    nx_result_t result;
    nx_ack_t ack;
    if (type == NX_MSG_HEARTBEAT) {
        if (!decode_heartbeat(buf, n, &hb, &ext)) return false;
    } else if (type == NX_MSG_ACK) {
        if (n != (int)sizeof(ack)) return false;
        memcpy(&ack, buf, sizeof(ack));
    } else if (type == NX_MSG_RESULT) {
        if (n < (int)sizeof(result)) return false;
        memcpy(&result, buf, sizeof(result));
        if (result.status > NX_RES_ERROR || result.data_len > NX_TILE_BYTES ||
            (result.status != NX_RES_OK && result.data_len != 0) ||
            n != (int)(sizeof(result) + result.data_len)) return false;
    } else {
        return false;
    }

    NODES_LOCK();
    node_t *nd = &s_nodes[slot];
    if (nd->epoch != epoch || nd->v.state == NODE_OFFLINE) {
        NODES_UNLOCK();
        return false;
    }
    bool recovered = nd->v.state == NODE_SUSPECT;
    nd->v.last_heard = esp_timer_get_time();
    if (recovered) nd->v.state = NODE_ONLINE;
    if (type == NX_MSG_HEARTBEAT) apply_heartbeat_locked(nd, &hb, &ext);
    NODES_UNLOCK();

    if (recovered) nx_log("SUP", NX_LOG_OK, "%s is talking again: ONLINE", nd->v.name);
    if (type == NX_MSG_ACK) jq_mark_running(s_gq, ack.id, ack.attempt, slot);
    else if (type == NX_MSG_RESULT) on_result(slot, &result, buf + sizeof(result));
    return true;
}

static void link_rx_task(void *arg)
{
    const uint8_t slot = (uint8_t)(uintptr_t)arg;
    node_t *nd = &s_nodes[slot];
    uint8_t *buf = malloc(NX_MAX_FRAME);
    if (!buf) {
        nx_log("MST", NX_LOG_ERR, "cannot allocate RX buffer");
        abort();
    }
    for (;;) {
        uint32_t epoch;
        xTaskNotifyWait(0, UINT32_MAX, &epoch, portMAX_DELAY);
        NODES_LOCK();
        int s = nd->sock;
        NODES_UNLOCK();
        char reason[70];
        for (;;) {
            uint8_t type;
            int n = nx_recv_frame(s, buf, NX_MAX_FRAME, &type);
            if (n < 0) {
                int error = errno;
                if (!error) strlcpy(reason, "TCP peer closed", sizeof(reason));
                else if (error == EBADMSG) strlcpy(reason, "invalid or partial NX frame", sizeof(reason));
                else snprintf(reason, sizeof(reason), "TCP receive errno=%d", error);
                break;
            }
            if (!handle_frame(slot, epoch, type, buf, n)) {
                snprintf(reason, sizeof(reason), "unexpected NX message type=%u length=%d", type, n);
                break;
            }
        }
        go_offline(slot, epoch, 0, reason);
        xSemaphoreTake(nd->io_mtx, portMAX_DELAY);
        NODES_LOCK();
        close(s);
        nd->sock = -1;
        NODES_UNLOCK();
        xSemaphoreGive(nd->io_mtx);
    }
}

static void link_tx_task(void *arg)
{
    const uint8_t slot = (uint8_t)(uintptr_t)arg;
    node_t *nd = &s_nodes[slot];
    link_msg_t m;
    for (;;) {
        xQueueReceive(nd->txq, &m, portMAX_DELAY);
        xSemaphoreTake(nd->io_mtx, portMAX_DELAY);
        NODES_LOCK();
        int s = nd->epoch == m.epoch && nd->v.state != NODE_OFFLINE ? nd->sock : -1;
        uint32_t hb_seq = nd->v.hb.seq;
        NODES_UNLOCK();
        if (s >= 0) {
            int64_t started = esp_timer_get_time();
            if (!nx_send_frame(s, m.type, m.payload, m.len, NULL, 0)) {
                int error = errno;
                nx_log("MST", NX_LOG_WARN, "%s TCP send type=%u errno=%d", nd->v.name, m.type, error);
                shutdown(s, SHUT_RDWR);  /* RX handles the offline transition once */
            } else if (m.type == NX_MSG_CONTROL) {
                nx_control_t c;
                memcpy(&c, m.payload, sizeof(c));
                NODES_LOCK();
                if (c.cmd == NX_CTL_SET_SLOTS) {
                    nd->sent_slot_generation = NX_SLOT_CONTROL_GENERATION(c.arg);
                    nd->sent_hb_seq = hb_seq;
                } else if (c.cmd == NX_CTL_REBOOT || c.cmd == NX_CTL_MUTE_LINK) {
                    nd->v.fault_at = started;
                    nd->active_fault_ticket = m.fault_ticket;
                }
                NODES_UNLOCK();
            }
        }
        xSemaphoreGive(nd->io_mtx);
    }
}

/* Keep enqueue atomic with session selection. All callers hold nodes_mtx. */
static bool post_locked(node_t *nd, uint8_t type, const void *payload, uint16_t len,
                        uint32_t fault_ticket)
{
    link_msg_t m = { .type = type, .len = len, .epoch = nd->epoch, .fault_ticket = fault_ticket };
    if (nd->sock < 0 || nd->v.state == NODE_OFFLINE || len > sizeof(m.payload)) return false;
    memcpy(m.payload, payload, len);
    return xQueueSend(nd->txq, &m, 0) == pdTRUE;
}

static bool send_slots_locked(node_t *nd, uint8_t k)
{
    uint32_t generation = (nd->slot_generation % 0xFFFFFFu) + 1;
    nx_control_t control = { .cmd = NX_CTL_SET_SLOTS, .arg = (generation << 8) | k };
    if (!post_locked(nd, NX_MSG_CONTROL, &control, sizeof(control), 0)) return false;
    nd->slot_generation = generation;
    nd->v.requested_k = k;
    nd->v.slots_ready = false;
    return true;
}

static void admit(int s, const nx_hello_t *h, uint32_t ip)
{
    int slot;
    bool known;
    int64_t close_deadline = 0;
    for (;;) {
        slot = -1;
        known = false;
        ROUTE_LOCK();
        NODES_LOCK();
        for (int i = 1; i < NX_NODES; i++) {
            if (s_nodes[i].v.state != NODE_EMPTY && memcmp(s_nodes[i].v.mac, h->mac, 6) == 0) {
                slot = i;
                known = true;
                break;
            }
        }
        for (int i = 1; i < NX_NODES && slot < 0; i++)
            if (s_nodes[i].v.state == NODE_EMPTY) slot = i;
        if (slot >= 0 && s_nodes[slot].sock < 0) break;

        uint32_t old_epoch = 0;
        bool rebooted = false;
        bool stale = false;
        if (slot >= 0) {
            const node_t *old = &s_nodes[slot];
            old_epoch = old->epoch;
            rebooted = old->v.inc != h->incarnation;
            stale = rebooted || esp_timer_get_time() - old->v.last_heard >= NX_SUSPECT_MS * 1000LL;
        }
        NODES_UNLOCK();
        ROUTE_UNLOCK();
        if (!stale || (close_deadline && esp_timer_get_time() >= close_deadline)) {
            close(s);  /* no room, or an active previous connection */
            return;
        }
        if (!close_deadline) {
            /* A restarted worker or a same-MAC reconnect after prolonged silence
             * can precede the old TCP EOF. Do not reuse the slot until its RX task
             * closes that descriptor. An epoch check protects racing teardown. */
            go_offline((uint8_t)slot, old_epoch, rebooted ? 0 : NX_SUSPECT_MS,
                       rebooted ? "worker rebooted" : "stale TCP replaced");
            close_deadline = esp_timer_get_time() + 250000;
        }
        vTaskDelay(1);
    }

    node_t *nd = &s_nodes[slot];
    uint8_t target_k = known ? nd->v.requested_k : h->slots_k;
    if (target_k < 1 || target_k > h->pool_n) target_k = h->slots_k;
    memcpy(nd->v.mac, h->mac, 6);
    nd->v.inc = h->incarnation;
    nd->v.ip = ip;
    nd->v.k = h->slots_k;
    nd->v.n = h->pool_n;
    nd->v.requested_k = target_k;
    nd->v.chip = NX_CHIP_UNKNOWN;
    nd->v.cores = 0;
    nd->v.cpu_mhz = 0;
    nd->v.state = NODE_ONLINE;
    nd->v.last_heard = esp_timer_get_time();
    nd->v.fault_at = 0;
    nd->v.hb_valid = false;
    memset(&nd->v.hb, 0, sizeof(nd->v.hb));
    nd->v.slots_ready = false;
    nd->v.slot_ack_supported = false;
    nd->sock = s;
    nd->epoch++;
    uint32_t epoch = nd->epoch;
    nd->sent_slot_generation = 0;
    nd->active_fault_ticket = 0;
    nd->reboot_pending = false;
    xQueueReset(nd->txq);
    nx_welcome_t welcome = { .node = (uint8_t)slot };
    (void)post_locked(nd, NX_MSG_WELCOME, &welcome, sizeof(welcome), 0);
    (void)send_slots_locked(nd, target_k);  /* confirm applied slots for this session */
    NODES_UNLOCK();
    xTaskNotify(nd->rx_task, epoch, eSetValueWithOverwrite);
    ROUTE_UNLOCK();

    nx_log("MST", NX_LOG_OK, "%s %s: MAC " MACSTR ", IP " IPSTR ", K = %u / N = %u",
           nd->v.name, known ? "reconnected" : "joined", MAC2STR(h->mac),
           IP2STR((esp_ip4_addr_t *)&ip), target_k, h->pool_n);
}

static void listener_task(void *arg)
{
    int one = 1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(NX_LINK_PORT),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    int listener;
    for (;;) {
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (listener >= 0) {
            setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            if (bind(listener, (struct sockaddr *)&a, sizeof(a)) == 0 && listen(listener, NX_MAX_WORKERS) == 0)
                break;
            close(listener);
        }
        nx_log("MST", NX_LOG_ERR, "worker listener unavailable, retrying");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int s = accept(listener, (struct sockaddr *)&peer, &plen);
        if (s < 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        struct timeval hello_timeout = { .tv_sec = 2 };
        struct timeval send_timeout = { .tv_usec = 750000 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &hello_timeout, sizeof(hello_timeout));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
        nx_hello_t h;
        uint8_t type;
        int n = nx_recv_frame(s, (uint8_t *)&h, sizeof(h), &type);
        if (n != (int)sizeof(h) || type != NX_MSG_HELLO || h.pool_n == 0 || h.pool_n > NX_POOL_SIZE ||
            h.slots_k == 0 || h.slots_k > h.pool_n) { close(s); continue; }
        struct timeval no_timeout = { 0 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &no_timeout, sizeof(no_timeout));
        admit(s, &h, peer.sin_addr.s_addr);
    }
}

/* ------------------------------ dispatcher ------------------------------ */

/* The routing mutex keeps routing policy stable until the job is enqueued. */
static int choose_node(void)
{
    nx_placement_node_t nodes[NX_NODES];
    NODES_LOCK();
    uint8_t placement = s_placement;
    bool local = s_local_on;
    for (int i = 0; i < NX_NODES; i++) {
        const node_t *node = &s_nodes[i];
        const nx_node_view_t *v = &node->v;
        nodes[i] = (nx_placement_node_t){
            .eligible = v->state == NODE_ONLINE && !v->drain && !node->reboot_pending,
            .k = v->k, .requested_k = v->requested_k,
            .cores = v->cores, .cpu_mhz = v->cpu_mhz,
        };
    }
    NODES_UNLOCK();
    for (int i = 0; i < NX_NODES; i++)
        if (nodes[i].eligible && local == (i == NX_NODE_LOCAL))
            nodes[i].inflight = jq_count_on_node(s_gq, (uint8_t)i);
    return nx_select_node(nodes, local, placement, s_rr);
}

static void send_dispatch(uint8_t node, const nx_job_t *job)
{
    bool ok;
    if (node == NX_NODE_LOCAL) {
        ok = jq_submit(&s_local_q, &job->spec, 0);
        if (ok) jq_mark_running(s_gq, job->spec.id, job->spec.attempt, node);
    } else {
        NODES_LOCK();
        ok = post_locked(&s_nodes[node], NX_MSG_DISPATCH, &job->spec, sizeof(job->spec), 0);
        NODES_UNLOCK();
    }
    if (!ok) {
        (void)jq_requeue_one(s_gq, job->spec.id, job->spec.attempt, node);
        return;
    }
    NODES_LOCK();
    s_nodes[node].v.dispatched++;
    NODES_UNLOCK();
}

static void dispatcher_task(void *arg)
{
    for (;;) {
        if (!jq_wait_ready(s_gq, portMAX_DELAY)) continue;
        for (;;) {
            ROUTE_LOCK();
            int node = choose_node();
            if (node >= 0) {
                NODES_LOCK();
                uint32_t inc = s_nodes[node].v.inc;
                NODES_UNLOCK();
                nx_job_t job;
                bool popped = jq_pop_best(s_gq, &job, (uint8_t)node, inc);
                if (popped) {
                    send_dispatch((uint8_t)node, &job);
                    s_rr = (uint8_t)node;
                }
                ROUTE_UNLOCK();
                if (!popped) vTaskDelay(1);
                break;
            }
            ROUTE_UNLOCK();
            vTaskDelay(1);  /* one tick works for both 100 Hz and 1000 Hz configs */
        }
    }
}

/* ------------------------------ supervisor ------------------------------ */

static void supervisor_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(NX_SUPERVISOR_TICK_MS));
        for (uint8_t i = 1; i < NX_NODES; i++) {
            NODES_LOCK();
            node_t *nd = &s_nodes[i];
            bool online = nd->v.state == NODE_ONLINE;
            bool active = online || nd->v.state == NODE_SUSPECT;
            uint32_t epoch = nd->epoch;
            int64_t now = esp_timer_get_time();
            int64_t silent_ms = (now - nd->v.last_heard) / 1000;
            bool offline = active && silent_ms >= NX_OFFLINE_MS;
            bool suspect = online && silent_ms >= NX_SUSPECT_MS && !offline;
            if (suspect) nd->v.state = NODE_SUSPECT;
            if (nd->reboot_pending && nd->v.fault_at > 0 && now - nd->v.fault_at > 5000000) {
                nd->reboot_pending = false;  /* a worker that ignored reboot may still serve jobs */
                nd->active_fault_ticket = 0;
                nd->v.fault_at = 0;
            }
            NODES_UNLOCK();
            if (offline) go_offline(i, epoch, NX_OFFLINE_MS, "no heartbeat");
            else if (suspect) nx_log("SUP", NX_LOG_WARN, "%s SUSPECT: silent for %lld ms",
                                     nd->v.name, silent_ms);
        }
        int expired = jq_requeue_expired(s_gq, esp_timer_get_time());
        if (expired) nx_log("SUP", NX_LOG_WARN, "%d job(s) requeued: lease expired", expired);
        jq_retry_pending(s_gq);
    }
}

/* ------------------------------ monitor ------------------------------ */

static void monitor_task(void *arg)
{
    static nx_monitor_t mon;
    static nx_heartbeat_t hb;
    for (;;) {
        monitor_sample(&mon);
        hb.seq++;
        hb.uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
        hb.heap_free = (uint32_t)esp_get_free_heap_size();
        hb.core_load[0] = mon.core_load[0];
        hb.core_load[1] = mon.core_load[1];
        hb.ntasks = mon.count;
        memcpy(hb.tasks, mon.task, sizeof(nx_task_info_t) * mon.count);
        oslab_get(&hb.lab);
        ROUTE_LOCK();
        taskENTER_CRITICAL(&s_local_pool.lock);
        hb.completed = s_local_pool.completed;
        hb.timeouts = s_local_pool.timeouts;
        hb.k = s_local_pool.target_k;
        hb.n = s_local_pool.n;
        hb.running = s_local_pool.running;
        hb.peak_running = s_local_pool.peak_running;
        hb.n_running_ids = 0;
        for (int j = 0; j < NX_POOL_SIZE; j++) {
            if (s_local_pool.running_id[j]) hb.running_ids[hb.n_running_ids++] = s_local_pool.running_id[j];
        }
        uint8_t permits = s_local_pool.permits;
        taskEXIT_CRITICAL(&s_local_pool.lock);
        NODES_LOCK();
        nx_node_view_t *local = &s_nodes[NX_NODE_LOCAL].v;
        local->hb = hb;
        local->hb_valid = true;
        local->k = permits;
        local->slots_ready = permits == local->requested_k;
        NODES_UNLOCK();
        ROUTE_UNLOCK();
        vTaskDelay(pdMS_TO_TICKS(NX_MONITOR_PERIOD_MS));
    }
}

/* ------------------------------ public API ------------------------------ */

void cluster_view(nx_node_view_t out[NX_NODES])
{
    NODES_LOCK();
    for (int i = 0; i < NX_NODES; i++) out[i] = s_nodes[i].v;
    NODES_UNLOCK();
}

void cluster_set_placement(uint8_t p)
{
    ROUTE_LOCK();
    NODES_LOCK();
    s_placement = p ? NX_PLACE_LEAST_LOADED : NX_PLACE_ROUND_ROBIN;
    s_rr = NX_NODE_LOCAL;
    NODES_UNLOCK();
    ROUTE_UNLOCK();
}

uint8_t cluster_placement(void)
{
    NODES_LOCK();
    uint8_t placement = s_placement;
    NODES_UNLOCK();
    return placement;
}

bool cluster_local(void)
{
    NODES_LOCK();
    bool local = s_local_on;
    NODES_UNLOCK();
    return local;
}

bool cluster_set_slots(uint8_t node, uint8_t k)
{
    if (node >= NX_NODES || k < 1 || k > NX_POOL_SIZE) return false;
    ROUTE_LOCK();
    node_t *nd = &s_nodes[node];
    bool ok;
    if (node == NX_NODE_LOCAL) {
        pool_set_slots(&s_local_pool, k);
        taskENTER_CRITICAL(&s_local_pool.lock);
        uint8_t permits = s_local_pool.permits;
        taskEXIT_CRITICAL(&s_local_pool.lock);
        NODES_LOCK();
        nd->v.requested_k = k;
        nd->v.k = permits;
        nd->v.slots_ready = permits == k;
        NODES_UNLOCK();
        ok = true;
    } else {
        NODES_LOCK();
        ok = nd->v.state != NODE_EMPTY && k <= nd->v.n;
        if (ok && (nd->v.state == NODE_OFFLINE || nd->reboot_pending)) {
            /* Restore the requested K after an experiment even if the worker
             * disconnected. Admission will send it when this MAC rejoins. */
            nd->v.requested_k = k;
            nd->v.slots_ready = false;
        } else if (ok) {
            ok = send_slots_locked(nd, k);
        }
        NODES_UNLOCK();
    }
    ROUTE_UNLOCK();
    if (ok) nx_log("MST", NX_LOG_INFO, "%s: requested execution permits K = %u", nd->v.name, k);
    return ok;
}

void cluster_set_drain(uint8_t node, bool on)
{
    if (node >= NX_NODES) return;
    ROUTE_LOCK();
    NODES_LOCK();
    bool changed = s_nodes[node].v.drain != on;
    s_nodes[node].v.drain = on;
    NODES_UNLOCK();
    ROUTE_UNLOCK();
    if (changed) nx_log("MST", NX_LOG_INFO, "%s %s", s_nodes[node].v.name,
                        on ? "draining: no new jobs" : "accepting jobs again");
}

void cluster_set_worker_drain(bool a, bool b, bool c)
{
    ROUTE_LOCK();
    NODES_LOCK();
    s_nodes[1].v.drain = a;
    s_nodes[2].v.drain = b;
    s_nodes[3].v.drain = c;
    NODES_UNLOCK();
    ROUTE_UNLOCK();
}

void cluster_set_local(bool on)
{
    ROUTE_LOCK();
    NODES_LOCK();
    bool changed = s_local_on != on;
    s_local_on = on;
    s_nodes[NX_NODE_LOCAL].v.state = on ? NODE_ONLINE : NODE_EMPTY;
    NODES_UNLOCK();
    ROUTE_UNLOCK();
    if (changed) nx_log("MST", NX_LOG_INFO, on ? "single-node mode ON: new jobs go to master"
                                               : "single-node mode OFF: new jobs go to workers");
}

bool cluster_mute(uint8_t node, uint32_t ms)
{
    if (node == NX_NODE_LOCAL || node >= NX_NODES || ms == 0) return false;
    nx_control_t control = { .cmd = NX_CTL_MUTE_LINK, .arg = ms };
    NODES_LOCK();
    node_t *nd = &s_nodes[node];
    bool ok = nd->v.state == NODE_ONLINE && !nd->reboot_pending &&
        post_locked(nd, NX_MSG_CONTROL, &control, sizeof(control), 0);
    NODES_UNLOCK();
    if (ok) nx_log("LAB", NX_LOG_WARN, "%s link mute requested (%lu ms)", nd->v.name, (unsigned long)ms);
    return ok;
}

uint32_t cluster_reboot(uint8_t node)
{
    if (node == NX_NODE_LOCAL || node >= NX_NODES) return 0;
    ROUTE_LOCK();
    NODES_LOCK();
    node_t *nd = &s_nodes[node];
    uint32_t ticket = ++s_fault_sequence;
    if (ticket == 0) ticket = ++s_fault_sequence;
    nx_control_t control = { .cmd = NX_CTL_REBOOT, .arg = ticket };
    bool ok = nd->v.state == NODE_ONLINE && !nd->reboot_pending &&
        post_locked(nd, NX_MSG_CONTROL, &control, sizeof(control), ticket);
    if (ok) nd->reboot_pending = true;
    NODES_UNLOCK();
    ROUTE_UNLOCK();
    if (ok) nx_log("LAB", NX_LOG_WARN, "%s restart requested", nd->v.name);
    return ok ? ticket : 0;
}

bool cluster_run_lab(uint8_t node, uint8_t lab)
{
    if (lab > 3 || node >= NX_NODES) return false;
    if (node == NX_NODE_LOCAL) return oslab_run(lab);
    nx_control_t control = { .cmd = NX_CTL_RUN_LAB, .arg = lab };
    NODES_LOCK();
    node_t *nd = &s_nodes[node];
    bool ok = nd->v.state == NODE_ONLINE && !nd->reboot_pending &&
        post_locked(nd, NX_MSG_CONTROL, &control, sizeof(control), 0);
    NODES_UNLOCK();
    return ok;
}

void cluster_start(nx_job_queue_t *gq)
{
    static const char *names[NX_NODES] = { "MASTER", "WORKER A", "WORKER B", "WORKER C" };
    s_gq = gq;
    s_nodes_mtx = xSemaphoreCreateMutex();
    s_route_mtx = xSemaphoreCreateMutex();
    if (!s_nodes_mtx || !s_route_mtx) abort();
    for (int i = 0; i < NX_NODES; i++) {
        strlcpy(s_nodes[i].v.name, names[i], sizeof(s_nodes[i].v.name));
        s_nodes[i].sock = -1;
        s_nodes[i].v.n = NX_POOL_SIZE;
    }
    esp_read_mac(s_nodes[0].v.mac, ESP_MAC_WIFI_SOFTAP);
    s_nodes[0].v.inc = esp_random();
    s_nodes[0].v.ip = ESP_IP4TOADDR(192, 168, 4, 1);
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    s_nodes[0].v.chip = chip.model == CHIP_ESP32S3 ? NX_CHIP_ESP32S3 : NX_CHIP_ESP32;
    s_nodes[0].v.cores = chip.cores;
    s_nodes[0].v.cpu_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    s_nodes[0].v.k = NX_DEFAULT_SLOTS;
    s_nodes[0].v.requested_k = NX_DEFAULT_SLOTS;
    s_nodes[0].v.slots_ready = true;
    s_nodes[0].v.slot_ack_supported = true;

    /* the master's own engine: same module as on the workers, off until enabled */
    if (!jq_init(&s_local_q, NX_LOCAL_QUEUE_CAP, 0)) abort();
    pool_start(&s_local_pool, &s_local_q, NX_DEFAULT_SLOTS, local_result, NULL);

    for (int i = 1; i < NX_NODES; i++) {
        char name[12];
        s_nodes[i].txq = xQueueCreate(8, sizeof(link_msg_t));
        s_nodes[i].io_mtx = xSemaphoreCreateMutex();
        if (!s_nodes[i].txq || !s_nodes[i].io_mtx) abort();
        snprintf(name, sizeof(name), "link_rx_%c", 'A' + i - 1);
        xTaskCreatePinnedToCore(link_rx_task, name, NX_STACK_LINK, (void *)(uintptr_t)i, NX_PRIO_LINK, &s_nodes[i].rx_task, tskNO_AFFINITY);
        snprintf(name, sizeof(name), "link_tx_%c", 'A' + i - 1);
        xTaskCreatePinnedToCore(link_tx_task, name, NX_STACK_LINK, (void *)(uintptr_t)i, NX_PRIO_LINK, NULL, tskNO_AFFINITY);
    }
    xTaskCreatePinnedToCore(listener_task, "listener", NX_STACK_LINK, NULL, NX_PRIO_LINK - 1, NULL, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(dispatcher_task, "dispatcher", NX_STACK_LINK, NULL, NX_PRIO_DISPATCH, NULL, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(supervisor_task, "supervisor", NX_STACK_SMALL, NULL, NX_PRIO_SUPERVISOR, NULL, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(monitor_task, "monitor", NX_STACK_LINK, NULL, NX_PRIO_MONITOR, NULL, tskNO_AFFINITY);
}
