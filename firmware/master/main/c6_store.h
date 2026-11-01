/* Shared state the C6 shows on its web: the local C5's latest status and a
 * short log of recent passes (local gate for now; the fleet hub adds remote
 * gates later). Mutex-guarded. */
#pragma once
#include "link_proto.h"
#include <stdbool.h>
#include <stdint.h>

#define C6_EVENTS 24u

typedef struct {
    uint8_t     channel;
    int32_t     peak_db_x10;
    uint32_t    at_ms;
    uint8_t     gate;        /* 0 = the master's local gate */
} c6_pass_t;

typedef struct {
    lp_stats_t  local;       /* local C5 stats */
    bool        has_local;
    uint32_t    total_passes;
    uint8_t     n_passes;    /* valid entries, newest first */
    c6_pass_t   passes[C6_EVENTS];
} c6_snapshot_t;

void c6_store_init(void);
void c6_store_set_local_stats(const lp_stats_t *s);
void c6_store_add_pass(const c6_pass_t *p);
void c6_store_snapshot(c6_snapshot_t *out);
