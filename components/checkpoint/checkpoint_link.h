/* Internal seam between checkpoint.c (detector + NVS state) and
 * checkpoint_link.c (the C5<->C3 UART link). Not a public component header. */
#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "link_proto.h"
#include "pass_detector.h"

/* Implemented in checkpoint_link.c, called by checkpoint.c. */
void checkpoint_link_start(void);               /* after the scan task is up */
void checkpoint_link_report_pass(const pd_event_t *e);   /* from the scan task */
TaskHandle_t checkpoint_link_task(void);        /* NULL when the link is off (T's stack line) */

/* Implemented in checkpoint.c, called by checkpoint_link.c. These reach into
 * the running detector state, so they are no-ops before checkpoint_start. */
bool checkpoint_link_apply_config(const lp_config_t *c); /* validate, post to the scan task, persist */
void checkpoint_link_get_config(lp_config_t *out);
void checkpoint_link_get_stats(lp_stats_t *out);         /* live rises, detector states, health */
void checkpoint_link_command(uint8_t cmd);               /* lp_cmd_t: recalibrate (B/F) or reboot */
