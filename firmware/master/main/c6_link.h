/* C6 side of the UART link to its local C5 (link_proto): receive the local
 * gate's passes, stats (with its health) and detector config, play the
 * centralized coin sound on a pass, and forward the admin's calibrate/reboot
 * commands to it. */
#pragma once
#include "link_proto.h"
void c6_link_start(void);
void c6_link_send_command(lp_cmd_t cmd);   /* LP_MSG_COMMAND to the local C5 */
/* Passage calibration of the local C5 (link_proto CALIB_*): step 0 ends the session, 1 passage, 2 away.
 * get: the C5's latest state of it, false without a session. apply: the C5's proposed thresholds as
 * a CONFIG_SET over its echoed config, then the session ends; false without a finished proposal. */
void c6_link_calib_start(uint8_t step);
bool c6_link_calib_get(lp_calib_t *out);
bool c6_link_calib_apply(void);
void c6_link_send_gain(uint8_t gain);      /* LP_MSG_GAIN to the local C5: LP_GAIN_MIN..LP_GAIN_MAX, it relearns its rests */
/* Gate 0 takes the fleet's re-arm drop (c6_store): at the next CONFIG_STATE, asked for now, a
 * CONFIG_SET of the C5's echoed settings with the fleet's drop, if its own differs. Also done
 * at boot and when the C5 says HELLO (it rebooted). */
void c6_link_sync_rearm(void);

/* Debug mode (the web's admin): the local C5's console text and typed keys. Off at every boot (the C5
 * switches off on our HELLO too): the log is kept and the keys go out only while it is on. The text of
 * its LOG frames sits in a ring, sanitized (lp_logring) so it can go in JSON; positions count bytes
 * since debug was switched on. */
#define C6_LOG_CHUNK 700u
void c6_link_debug_set(bool on);
bool c6_link_debug(void);
/* Copies up to max bytes from position `since`; *next is where to continue, *lost that the ring had
 * dropped bytes after `since` (skipped). A position past the end restarts at the oldest byte kept. */
size_t c6_link_log_read(uint32_t since, char *out, size_t max, uint32_t *next, bool *lost);
void c6_link_send_keys(const char *keys, size_t n);   /* KEY frame: typed on the C5's console */
