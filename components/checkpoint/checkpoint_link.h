/* Internal seam between checkpoint.c (detector + NVS state) and
 * checkpoint_link.c (the C5<->C3 UART link). Not a public component header. */
#pragma once
#include "link_proto.h"
#include "pass_detector.h"

/* Implemented in checkpoint_link.c, called by checkpoint.c. */
void checkpoint_link_start(void);               /* after the task is up */
void checkpoint_link_report_pass(const pd_event_t *e);

/* Implemented in checkpoint.c, called by checkpoint_link.c. These reach into
 * the running detector state, so they are no-ops before checkpoint_start. */
bool checkpoint_link_apply_config(const lp_config_t *c); /* validate, apply, persist */
void checkpoint_link_get_config(lp_config_t *out);
void checkpoint_link_get_stats(lp_stats_t *out);         /* live C/N, detector state, health */
void checkpoint_link_command(uint8_t cmd);               /* lp_cmd_t: calibrate (B/F) or reboot */
