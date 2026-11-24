/*
 * worker_link.c — message passing between this worker and the master.
 *
 * Two tasks own the connection:
 *   link_rx  connects (with back-off), says HELLO, then receives DISPATCH and
 *            CONTROL messages. A DISPATCH is pushed into the local job queue
 *            (this task is the producer of the worker's bounded buffer).
 *   link_tx  the ONLY task that sends on the socket. It sends whatever the
 *            executors queued (ACK, RESULT) and, every 500 ms, a heartbeat
 *            with progress: free slots, running jobs and the task table.
 */
#include <errno.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "logger.h"
#include "monitor.h"
#include "oslab.h"
#include "proto.h"
#include "wifi_sta.h"
#include "worker_link.h"

typedef struct {
    uint8_t  type;
    uint16_t len;
    uint8_t  payload[sizeof(nx_result_t) + NX_TILE_BYTES];
} out_msg_t;

#define TX_BUFFER_COUNT (NX_POOL_SIZE + 1) /* at most one prefetched job per board */
#define TX_TIMEOUT_MS 1500u

static QueueHandle_t   s_txq, s_free_tx;
static out_msg_t       s_buffers[TX_BUFFER_COUNT];
static SemaphoreHandle_t s_socket_lock, s_slot_lock;
static int             s_sock = -1;
static uint32_t        s_wifi_generation;
static int64_t         s_mute_until;
static nx_job_queue_t *s_q;
static nx_pool_t      *s_pool;
static uint32_t        s_incarnation;
static uint32_t        s_slot_ack;
static uint8_t         s_mac[6];
static esp_chip_info_t s_chip;

static out_msg_t *take_buffer(TickType_t wait)
{
    out_msg_t *m = NULL;
    xQueueReceive(s_free_tx, &m, wait);
    return m;
}

static void queue_buffer(out_msg_t *m)
{
    /* A free buffer was removed first, so there is space in the pending queue. */
    BaseType_t queued = xQueueSend(s_txq, &m, 0);
    configASSERT(queued == pdTRUE);
    (void)queued;
}

void worker_link_on_result(const nx_job_t *job, const nx_result_t *res, const uint8_t *data, void *arg)
{
    if (res->data_len > NX_TILE_BYTES) return;
    /* Limit back-pressure so a dead link cannot block all four executors. */
    out_msg_t *m = take_buffer(pdMS_TO_TICKS(250));
    if (!m) {
        nx_log("WRK", NX_LOG_WARN, "result buffer full for job #%lu", (unsigned long)res->id);
        return;                            /* the master retries on lease expiry */
    }
    m->type = NX_MSG_RESULT;
    m->len = (uint16_t)(sizeof(*res) + res->data_len);
    memcpy(m->payload, res, sizeof(*res));
    if (res->data_len) memcpy(m->payload + sizeof(*res), data, res->data_len);
    queue_buffer(m);
}

typedef enum { LINK_SENT, LINK_MUTED, LINK_LOST } send_status_t;

static send_status_t send_packet(uint8_t type, const void *a, uint16_t alen, const void *b, uint16_t blen)
{
    send_status_t status = LINK_LOST;
    xSemaphoreTake(s_socket_lock, portMAX_DELAY);
    if (s_sock >= 0) {
        uint32_t gateway, generation;
        if (!wifi_sta_link(&gateway, &generation) || generation != s_wifi_generation) {
            nx_log("WRK", NX_LOG_WARN, "discarding socket from previous Wi-Fi association");
            shutdown(s_sock, SHUT_RDWR);
        } else if (esp_timer_get_time() < s_mute_until) {
            status = LINK_MUTED;
        } else if (nx_send_frame(s_sock, type, a, alen, b, blen)) {
            status = LINK_SENT;
        } else {
            int error = errno;
            nx_log("WRK", NX_LOG_WARN, "frame TX failed: type %u errno %d (%s)", type, error, strerror(error));
            shutdown(s_sock, SHUT_RDWR);     /* wake link_rx; it owns close() */
        }
    }
    xSemaphoreGive(s_socket_lock);
    return status;
}

static void send_heartbeat(void)
{
    static nx_heartbeat_t hb;
    static nx_monitor_t mon;
    static uint32_t seq;

    xSemaphoreTake(s_socket_lock, portMAX_DELAY);
    bool connected = s_sock >= 0;
    xSemaphoreGive(s_socket_lock);
    if (!connected) return;

    monitor_sample(&mon);
    memset(&hb, 0, sizeof(hb));
    hb.seq = ++seq;
    hb.uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
    hb.heap_free = (uint32_t)esp_get_free_heap_size();
    nx_heartbeat_ext_t ext = {
        .version = NX_HB_EXT_VERSION,
        .chip = s_chip.model == CHIP_ESP32S3 ? NX_CHIP_ESP32S3 : NX_CHIP_ESP32,
        .cores = configNUMBER_OF_CORES,
        .cpu_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
    };
    xSemaphoreTake(s_slot_lock, portMAX_DELAY);
    taskENTER_CRITICAL(&s_pool->lock);
    hb.completed = s_pool->completed;
    hb.timeouts = s_pool->timeouts;
    hb.k = s_pool->target_k;
    hb.n = s_pool->n;
    hb.running = s_pool->running;
    hb.peak_running = s_pool->peak_running;
    ext.slots_effective = s_pool->permits;
    ext.slot_ack = s_slot_ack;
    uint32_t ids[NX_POOL_SIZE] = { 0 };  /* packed wire fields cannot be used as array arguments */
    for (uint8_t i = 0; i < s_pool->n; i++) {
        if (s_pool->running_id[i]) ids[hb.n_running_ids++] = s_pool->running_id[i];
    }
    taskEXIT_CRITICAL(&s_pool->lock);
    xSemaphoreGive(s_slot_lock);
    hb.core_load[0] = mon.core_load[0];
    hb.core_load[1] = mon.core_load[1];
    memcpy(hb.running_ids, ids, sizeof(ids));
    hb.ntasks = mon.count;
    memcpy(hb.tasks, mon.task, sizeof(nx_task_info_t) * mon.count);
    oslab_get(&hb.lab);
    send_packet(NX_MSG_HEARTBEAT, &hb, NX_HEARTBEAT_LEN(hb.ntasks), &ext, sizeof(ext));
}

static void link_tx_task(void *arg)
{
    out_msg_t *m = NULL;
    int64_t next_hb = 0;
    for (;;) {
        int64_t now = esp_timer_get_time();
        if (now >= next_hb) {
            send_heartbeat();
            next_hb = now + (int64_t)NX_HEARTBEAT_MS * 1000;
        }
        now = esp_timer_get_time();
        TickType_t wait = next_hb > now ? pdMS_TO_TICKS((next_hb - now) / 1000) + 1 : 0;
        if (!m && xQueueReceive(s_txq, &m, wait) != pdTRUE) continue;
        if (send_packet(m->type, m->payload, m->len, NULL, 0) == LINK_MUTED) {
            vTaskDelay(pdMS_TO_TICKS(20));   /* keep this buffer until the fault ends */
        } else {
            xQueueSend(s_free_tx, &m, 0);
            m = NULL;
        }
    }
}

static void handle_control(const nx_control_t *c)
{
    switch (c->cmd) {
    case NX_CTL_SET_SLOTS: {
        uint8_t k = (uint8_t)c->arg;
        if (k < 1 || k > NX_POOL_SIZE) break;
        xSemaphoreTake(s_slot_lock, portMAX_DELAY);
        pool_set_slots(s_pool, k);
        s_slot_ack = NX_SLOT_CONTROL_GENERATION(c->arg);
        xSemaphoreGive(s_slot_lock);
        nx_log("WRK", NX_LOG_INFO, "execution permits K = %u", k);
        break;
    }
    case NX_CTL_MUTE_LINK:
        xSemaphoreTake(s_socket_lock, portMAX_DELAY);
        s_mute_until = esp_timer_get_time() + (int64_t)c->arg * 1000;
        xSemaphoreGive(s_socket_lock);
        nx_log("WRK", NX_LOG_WARN, "link muted for %lu ms (fault injection)", (unsigned long)c->arg);
        break;
    case NX_CTL_REBOOT:
        nx_log("WRK", NX_LOG_ERR, "restarting (fault injection)");
        esp_restart();
        break;
    case NX_CTL_RUN_LAB:
        oslab_run((uint8_t)c->arg);
        break;
    }
}

static int connect_master(uint32_t gateway)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(NX_LINK_PORT) };
    a.sin_addr.s_addr = gateway;
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s < 0) return -1;
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));   /* small messages: send at once */
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    int keep_idle = 4, keep_interval = 2, keep_probes = 3;
    setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &keep_idle, sizeof(keep_idle));
    setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &keep_interval, sizeof(keep_interval));
    setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, &keep_probes, sizeof(keep_probes));
    if (fcntl(s, F_SETFL, O_NONBLOCK) != 0) goto fail;
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        if (errno != EINPROGRESS) goto fail;
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(s, &writable);
        struct timeval timeout = { .tv_sec = 2 };
        int socket_error = 0;
        socklen_t len = sizeof(socket_error);
        if (select(s + 1, NULL, &writable, NULL, &timeout) <= 0 ||
            getsockopt(s, SOL_SOCKET, SO_ERROR, &socket_error, &len) != 0 || socket_error != 0) goto fail;
    }
    if (fcntl(s, F_SETFL, 0) != 0) goto fail;
    struct timeval send_timeout = { .tv_sec = TX_TIMEOUT_MS / 1000, .tv_usec = TX_TIMEOUT_MS % 1000 * 1000 };
    if (setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout)) != 0) goto fail;
    return s;

fail:
    close(s);
    return -1;
}

static bool register_worker(int sock, uint8_t *buffer, uint8_t *node)
{
    struct timeval timeout = { .tv_sec = 2 };
    nx_hello_t hello = { .incarnation = s_incarnation, .pool_n = NX_POOL_SIZE, .slots_k = s_pool->target_k };
    memcpy(hello.mac, s_mac, sizeof(s_mac));
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        !nx_send_frame(sock, NX_MSG_HELLO, &hello, sizeof(hello), NULL, 0)) return false;
    uint8_t type;
    int n = nx_recv_frame(sock, buffer, NX_MAX_FRAME, &type);
    if (n < (int)sizeof(nx_welcome_t) || type != NX_MSG_WELCOME ||
        buffer[0] == NX_NODE_LOCAL || buffer[0] > NX_MAX_WORKERS) return false;
    timeout = (struct timeval){ 0 };
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) return false;
    *node = buffer[0];
    return true;
}

static void link_rx_task(void *arg)
{
    static uint8_t buf[NX_MAX_FRAME];
    uint32_t backoff_ms = 500;

    for (;;) {
        wifi_sta_wait_connected(portMAX_DELAY);
        uint32_t gateway, generation;
        if (!wifi_sta_link(&gateway, &generation)) continue;
        int s = connect_master(gateway);
        uint8_t node;
        if (s < 0 || !register_worker(s, buf, &node)) {
            if (s >= 0) close(s);
            /* Exponential backoff with jitter keeps workers from retrying in lockstep. */
            vTaskDelay(pdMS_TO_TICKS(backoff_ms + esp_random() % 250));
            backoff_ms = backoff_ms * 2 > 8000 ? 8000 : backoff_ms * 2;
            continue;
        }
        xSemaphoreTake(s_slot_lock, portMAX_DELAY);
        s_slot_ack = 0;
        xSemaphoreGive(s_slot_lock);
        xSemaphoreTake(s_socket_lock, portMAX_DELAY);
        s_wifi_generation = generation;
        s_sock = s;                          /* from now on only link_tx sends on it */
        xSemaphoreGive(s_socket_lock);
        backoff_ms = 500;
        nx_log("WRK", NX_LOG_OK, "registered as node %u, incarnation %08lx", node, (unsigned long)s_incarnation);

        int receive_error;
        for (;;) {
            uint8_t type;
            errno = 0;
            int n = nx_recv_frame(s, buf, sizeof(buf), &type);
            if (n < 0) {
                receive_error = errno;
                break;
            }
            if (type == NX_MSG_DISPATCH && n >= (int)sizeof(nx_job_spec_t)) {
                nx_job_spec_t spec;
                memcpy(&spec, buf, sizeof(spec));
                /* producer side of the local bounded buffer; credits keep it from filling */
                if (jq_submit(s_q, &spec, 0)) {
                    out_msg_t *ack = take_buffer(0);
                    if (ack) {
                        nx_ack_t a = { .id = spec.id, .attempt = spec.attempt };
                        ack->type = NX_MSG_ACK;
                        ack->len = sizeof(a);
                        memcpy(ack->payload, &a, sizeof(a));
                        queue_buffer(ack);
                    }
                } else {
                    nx_log("WRK", NX_LOG_WARN, "local queue full, job #%lu dropped", (unsigned long)spec.id);
                }
            } else if (type == NX_MSG_CONTROL && n >= (int)sizeof(nx_control_t)) {
                nx_control_t c;
                memcpy(&c, buf, sizeof(c));
                handle_control(&c);
            }
        }
        xSemaphoreTake(s_socket_lock, portMAX_DELAY);
        s_sock = -1;
        xSemaphoreGive(s_socket_lock);
        close(s);
        if (receive_error) {
            nx_log("WRK", NX_LOG_WARN, "frame RX failed: errno %d (%s), reconnecting",
                   receive_error, strerror(receive_error));
        } else {
            nx_log("WRK", NX_LOG_WARN, "master socket closed, reconnecting");
        }
        vTaskDelay(pdMS_TO_TICKS(backoff_ms + esp_random() % 250));
    }
}

void worker_link_init(nx_job_queue_t *q, nx_pool_t *pool)
{
    s_q = q;
    s_pool = pool;
    s_txq = xQueueCreate(TX_BUFFER_COUNT, sizeof(out_msg_t *));
    s_free_tx = xQueueCreate(TX_BUFFER_COUNT, sizeof(out_msg_t *));
    s_socket_lock = xSemaphoreCreateMutex();
    s_slot_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_txq && s_free_tx && s_socket_lock && s_slot_lock ? ESP_OK : ESP_ERR_NO_MEM);
    for (size_t i = 0; i < TX_BUFFER_COUNT; i++) {
        out_msg_t *m = &s_buffers[i];
        xQueueSend(s_free_tx, &m, 0);
    }
}

void worker_link_start(void)
{
    s_incarnation = esp_random();           /* new number at every boot */
    esp_chip_info(&s_chip);
    nx_log("SYS", NX_LOG_INFO, "chip cores: %u physical, %u active", s_chip.cores, (unsigned)configNUMBER_OF_CORES);
    ESP_ERROR_CHECK(esp_read_mac(s_mac, ESP_MAC_WIFI_STA));
    BaseType_t rx = xTaskCreate(link_rx_task, "link_rx", NX_STACK_LINK, NULL, NX_PRIO_LINK, NULL);
    BaseType_t tx = xTaskCreate(link_tx_task, "link_tx", NX_STACK_LINK, NULL, NX_PRIO_LINK, NULL);
    ESP_ERROR_CHECK(rx == pdPASS && tx == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
