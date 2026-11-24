/*
 * kernels.c — FRACTAL, MATMUL and SPIN jobs.
 * Both supported targets have a single-precision FPU. Contraction is disabled
 * in CMake so tile arithmetic rounds the same way on LX6 and LX7.
 */
#include "esp_timer.h"
#include "esp_rom_crc.h"
#include "nexus_config.h"
#include "kernels.h"

static nx_result_status_t run_fractal(const nx_job_spec_t *s, uint8_t *out, int64_t deadline)
{
    const float x0 = s->p.fractal.x0, y0 = s->p.fractal.y0, step = s->p.fractal.step;
    const uint32_t max_it = s->p.fractal.max_iter ? s->p.fractal.max_iter : 1;

    for (int py = 0; py < NX_TILE; py++) {
        const float ci = y0 + py * step;
        for (int px = 0; px < NX_TILE; px++) {
            const float cr = x0 + px * step;
            float zr = 0, zi = 0, zr2 = 0, zi2 = 0;
            uint32_t it = 0;
            while (zr2 + zi2 <= 4.0f && it < max_it) {    /* z = z^2 + c */
                zi = 2.0f * zr * zi + ci;
                zr = zr2 - zi2 + cr;
                zr2 = zr * zr;
                zi2 = zi * zi;
                it++;
            }
            out[py * NX_TILE + px] = (it >= max_it) ? 255 : (uint8_t)(it * 254u / max_it);
        }
        if (esp_timer_get_time() > deadline) return NX_RES_TIMEOUT;   /* cooperative cancel */
    }
    return NX_RES_OK;
}

static uint32_t xorshift32(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

static nx_result_status_t run_matmul(const nx_job_spec_t *s, uint8_t *scratch, uint32_t *crc, int64_t deadline)
{
    int n = s->p.matmul.n;
    if (n < 1) n = 1;
    if (n > NX_MATMUL_MAX_N) n = NX_MATMUL_MAX_N;
    int32_t *row = (int32_t *)scratch;
    uint8_t *a = scratch + NX_MATMUL_MAX_N * sizeof(*row);
    uint8_t *bt = a + n * n;
    uint32_t seed = s->p.matmul.seed ? s->p.matmul.seed : 1;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            a[i * n + j] = (uint8_t)(xorshift32(&seed) % 100);
            bt[j * n + i] = (uint8_t)(xorshift32(&seed) % 100);
        }
    }

    uint32_t sum_crc = 0;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            int32_t sum = 0;
            for (int k = 0; k < n; k++) sum += a[i * n + k] * bt[j * n + k];
            row[j] = sum;
        }
        if (esp_timer_get_time() > deadline) return NX_RES_TIMEOUT;
        sum_crc = esp_rom_crc32_le(sum_crc, (const uint8_t *)row, n * sizeof(*row));
    }
    *crc = sum_crc;
    return NX_RES_OK;
}

static nx_result_status_t run_spin(const nx_job_spec_t *s, int64_t deadline)
{
    const int64_t end = esp_timer_get_time() + (int64_t)s->p.spin.duration_ms * 1000;
    volatile uint32_t work = 0;
    while (esp_timer_get_time() < end) {
        for (int i = 0; i < 1000; i++) work += i;
        if (esp_timer_get_time() > deadline) return NX_RES_TIMEOUT;
    }
    return NX_RES_OK;
}

nx_result_status_t nx_run_job(const nx_job_spec_t *spec, uint8_t *scratch, uint16_t *out_len,
                              uint32_t *checksum, int64_t deadline_us)
{
    nx_result_status_t r = NX_RES_ERROR;
    *out_len = 0;
    *checksum = 0;
    switch (spec->type) {
    case NX_JOB_FRACTAL:
        r = run_fractal(spec, scratch, deadline_us);
        if (r == NX_RES_OK) {
            *out_len = NX_TILE_BYTES;
            *checksum = esp_rom_crc32_le(0, scratch, NX_TILE_BYTES);
        }
        break;
    case NX_JOB_MATMUL:
        r = run_matmul(spec, scratch, checksum, deadline_us);
        break;
    case NX_JOB_SPIN:
        r = run_spin(spec, deadline_us);
        break;
    }
    return r;
}
