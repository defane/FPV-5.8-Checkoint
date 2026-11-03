/* Minimal web UI: one embedded page plus /api/state (GET snapshot incl. the
 * C5's health), /api/config (POST new configuration, pushed to the C5 and
 * saved) and /api/command (POST calibrate/reboot to the C5; admin only). */
#pragma once
void c3_web_start(void);
