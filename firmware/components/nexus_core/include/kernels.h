/*
 * kernels.h — the three job types. They are plain C, deterministic for a given
 * job, and check the clock regularly so a job that runs past its execution
 * budget cancels itself (task timeout handling, A2).
 */
#pragma once
#include <stdint.h>
#include "nexus_types.h"

/* Runs one job. `scratch` must hold NX_SCRATCH_BYTES. For FRACTAL jobs the tile
 * (NX_TILE_BYTES, one byte per pixel: 255 = inside the set) is left in scratch
 * and *out_len is set. Returns NX_RES_TIMEOUT if `deadline_us` passed. */
nx_result_status_t nx_run_job(const nx_job_spec_t *spec, uint8_t *scratch, uint16_t *out_len,
                              uint32_t *checksum, int64_t deadline_us);
