/* C3 side of the C5<->C3 UART link (link_proto): receive events/stats/config
 * state from the C5 into the store, and push the authoritative config down. */
#pragma once
#include "link_proto.h"

void c3_link_start(void);
void c3_link_push_config(const lp_config_t *c);   /* queue a CONFIG_SET to the C5 */
