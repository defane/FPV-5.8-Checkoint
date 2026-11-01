/* C3 side of the UART link to its local C5 (link_proto): deliver the C5's
 * passes/stats to registered handlers (fleet relay, or the degraded local
 * store), and push configuration / channel retune down to the C5. */
#pragma once
#include "link_proto.h"

void c3_link_set_handlers(void (*on_pass)(const lp_event_t *),
                          void (*on_stats)(const lp_stats_t *));
void c3_link_start(void);
void c3_link_push_config(const lp_config_t *c);   /* CONFIG_SET to the C5 */
void c3_link_set_channel(uint8_t rf_index);       /* SET_CHANNEL (retune) to the C5 */
