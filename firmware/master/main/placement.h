#pragma once
#include "nexus_config.h"
#include "nexus_types.h"

#define NX_NODES (1 + NX_MAX_WORKERS)

typedef struct {
    bool eligible;
    uint8_t k, requested_k, cores;
    uint16_t cpu_mhz, inflight;
} nx_placement_node_t;

/* K + 1 bounds prefetched work. The least-loaded score compares the queue after
 * admitting one new job, per physical compute capacity. Clock speed is a rough
 * throughput estimate, and round-robin breaks ties. */
static inline int nx_select_node(const nx_placement_node_t nodes[NX_NODES], bool master_only,
                                 uint8_t policy, uint8_t cursor)
{
    int best = -1;
    uint32_t best_capacity = 0;
    uint16_t best_projected = 0;
    for (int step = 1; step <= NX_NODES; step++) {
        uint8_t index = (cursor + step) % NX_NODES;
        if (master_only != (index == NX_NODE_LOCAL)) continue;
        const nx_placement_node_t *node = &nodes[index];
        uint8_t k = node->k < node->requested_k ? node->k : node->requested_k;
        if (!node->eligible || !k || node->inflight >= k + 1) continue;
        if (policy == NX_PLACE_ROUND_ROBIN) return index;

        uint8_t cores = node->cores ? node->cores : 2;
        uint32_t capacity = (k < cores ? k : cores) * (node->cpu_mhz ? node->cpu_mhz : 160u);
        uint16_t projected = node->inflight + 1;
        if (best < 0 || projected * best_capacity < best_projected * capacity) {
            best = index;
            best_capacity = capacity;
            best_projected = projected;
        }
    }
    return best;
}
