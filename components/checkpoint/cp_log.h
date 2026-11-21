/* Console text mirrored to the bridge (debug mode of the C3/C6 web). cp_printf() prints like
 * printf() and also copies the text into a small ring on the heap; the link task drains it into
 * LP_MSG_LOG frames. The ring drops the newest text when full (a bench or a trace flood), and the
 * link says how much was lost. Before cp_log_start() (no link, no memory) it is a plain printf. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int cp_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void cp_log_enable(bool on);                               /* debug mode: off by default; clears the ring */
bool cp_log_enabled(void);
bool cp_log_start(void);                                   /* allocates the ring; false: no memory */
size_t cp_log_take(char *out, size_t max, uint32_t *lost); /* up to max bytes; *lost: bytes dropped since the last call */
