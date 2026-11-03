/* Shared state the C6 shows on its web: the local C5's latest status and a
 * short log of recent passes (local gate for now; the fleet hub adds remote
 * gates later). Mutex-guarded. */
#pragma once
#include "link_proto.h"
#include <stdbool.h>
#include <stdint.h>

#define C6_EVENTS 24u
#define C6_LOCAL_LIVE_MS 3000u   /* no local STATS this long (3 periods) -> the local C5 is absent */

typedef struct {
    uint8_t     channel;
    int32_t     peak_db_x10;
    uint32_t    at_ms;
    uint8_t     gate;        /* 0 = the master's local gate */
} c6_pass_t;

typedef struct {
    lp_stats_t  local;       /* local C5 stats and health */
    bool        has_local;   /* in a snapshot: a local STATS within C6_LOCAL_LIVE_MS */
    lp_config_t local_cfg;   /* the local C5's detector config (its CONFIG_STATE echo) */
    bool        has_local_cfg;
    uint32_t    total_passes;
    uint8_t     n_passes;    /* valid entries, newest first */
    c6_pass_t   passes[C6_EVENTS];
} c6_snapshot_t;

void c6_store_init(void);
void c6_store_set_local_stats(const lp_stats_t *s);
void c6_store_set_local_config(const lp_config_t *c);
bool c6_store_has_local(void);    /* the local C5 reported within C6_LOCAL_LIVE_MS */
void c6_store_add_pass(const c6_pass_t *p);
void c6_store_snapshot(c6_snapshot_t *out);
