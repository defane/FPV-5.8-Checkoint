/* Authoritative state the C3 holds for the platform: the detector/audio
 * configuration (persisted in NVS), the latest stats from the C5 and a short
 * log of recent pass events. All access is mutex-guarded. */
#pragma once
#include "link_proto.h"
#include <stdbool.h>
#include <stdint.h>

#define C3_EVENTS 16u   /* recent passes kept for the web log */

typedef struct {
    lp_config_t config;
    bool        has_config;     /* false until adopted from the C5 (CONFIG_STATE) or set by the user */
    lp_stats_t  stats;
    bool        has_stats;
    uint32_t    total_events;
    uint8_t     n_events;       /* valid entries in events[], newest first */
    lp_event_t  events[C3_EVENTS];
} c3_snapshot_t;

void c3_store_init(void);                   /* mutex + load config from NVS */
lp_config_t c3_config_default(void);        /* mirrors pass_detector defaults + 60 % volume */

/* Returns false (and leaves *out at defaults) when no config has been set yet. */
bool c3_store_get_config(lp_config_t *out);
void c3_store_set_config(const lp_config_t *c, bool persist);

void c3_store_set_stats(const lp_stats_t *s);
void c3_store_add_event(const lp_event_t *e);

void c3_store_snapshot(c3_snapshot_t *out);
