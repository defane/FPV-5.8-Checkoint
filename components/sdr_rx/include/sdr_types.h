/* sdr_rx shared types: no ESP-IDF include (host-tested code uses them). GPL-3.0-only. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SDR_RX_CHANNELS 8u          /* R1..R8 = 0..7 */
#define SDR_RX_MAX_HOPS 8u          /* SINGLE mode */
#define SDR_RX_PER_HOP  2u          /* readings of one hop at most (PAIRS) */
#define SDR_LEVEL_NONE  INT16_MIN   /* no reading */
#define SDR_QDB_PER_DB  4           /* level_qdb = round(4 * dBFS) */

/* sdr_reading_t.flags */
#define SDR_RD_SATURATED 0x01u      /* an ADC rail was hit in this capture */
#define SDR_RD_DC_MASKED 0x02u      /* a DC-guard bin fell in the window (filled with the window mean) */

typedef enum { SDR_PLAN_PAIRS = 0, SDR_PLAN_SINGLE = 1 } sdr_plan_mode_t;

/* Plain (not averaged) centred I/Q moments, summed over segments: what the FULL verify and
 * K x accumulate (psd_moments_add). Per segment of nfft samples: nfft*sum(x*y) - sum(x)*sum(y).
 * saturated counts the captures left out because a sample hit an ADC rail. */
typedef struct { int64_t cii, cqq, ciq; uint32_t segments, nfft, saturated; } sdr_moments_t;
