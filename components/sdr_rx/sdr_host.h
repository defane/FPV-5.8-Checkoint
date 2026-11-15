/* sdr_rx: what the host-tested files (sdr_psd.c, sdr_level.c) take from ESP-IDF, with stand-ins
 * for host builds (test/), where every timing field reads 0 us. Private. GPL-3.0-only. */
#pragma once
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_timer.h"
static inline int64_t sdr_now_us(void) { return esp_timer_get_time(); }
#else
#ifndef IRAM_ATTR
#define IRAM_ATTR /* host builds (test/) */
#endif
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
static inline int64_t sdr_now_us(void) { return 0; }
#endif
