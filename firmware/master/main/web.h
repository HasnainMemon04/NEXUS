/*
 * web.h — the dashboard server on the master.
 *   GET /             NEXUS Control Center (gzip page from flash)
 *   GET /ws           WebSocket: state pushed up to four times per second
 *   GET /api/state    the same state once (JSON)
 *   GET /api/tiles    fractal tiles committed since a version (binary)
 *   GET /api/cmd      controls (policy, slots, faults, labs ...)
 *   GET /api/csv      one row per committed job (with its run id), for the report's graphs
 *   GET /api/runs.csv one summary row per experiment run (experiments.c)
 */
#pragma once
#include "job_queue.h"

void web_start(nx_job_queue_t *gq);
