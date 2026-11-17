/* C3 side of the UART link to its local C5 (link_proto): deliver the C5's
 * passes/stats to registered handlers (fleet relay, or the degraded local
 * store), adopt the C5's CONFIG_STATE into c3_store (asked for every second and on
 * its HELLO, or the echo of the master's config a relay pushes then), and push
 * configuration / channel retune / commands down to the C5. */
#pragma once
#include "link_proto.h"
#include <stdbool.h>

void c3_link_set_handlers(void (*on_pass)(const lp_event_t *),
                          void (*on_stats)(const lp_stats_t *));
void c3_link_start(void);
void c3_link_push_config(const lp_config_t *c);   /* CONFIG_SET to the C5 */
/* Relay mode: the master's CONFIG for this gate. Pushed now (CONFIG_SET), and kept (RAM) to push
 * again when the C5 says HELLO: it booted after this one went out (lost), or it was swapped. */
void c3_link_set_master_config(const lp_config_t *c);
void c3_link_send_command(lp_cmd_t cmd);          /* COMMAND (console B/F, reboot) to the C5 */
/* A valid frame from the C5 within the last 3 s (it STATS every second);
 * false when it never spoke, or went silent (unplugged, rebooting, hung, v1). */
bool c3_link_c5_alive(void);
/* The re-arm drop *10 of the C5's last CONFIG_STATE (what it runs, polled every second, so a
 * console K e/E shows), -1 before its first one. Link task only (the fleet relay's NODE_STATUS). */
int32_t c3_link_c5_rearm_x10(void);
