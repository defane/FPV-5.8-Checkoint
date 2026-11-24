/* Firmware identity: the FIRMWARE line and the fw= field of log lines and STATS. After C5VRX-5
 * console.c firmware_id()/firmware_print() (Twotoz and the C5VRX contributors,
 * https://github.com/Twotoz/C5VRX, 6bc9e4b, GPL-3.0-only). LETZTIMER_COMMIT is defined for this
 * file only (CMakeLists.txt), so a new commit recompiles nothing else. */
#include <stdio.h>
#include "letztimer.h"
#include "lt_log.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"

#ifndef LETZTIMER_COMMIT
#define LETZTIMER_COMMIT "unknown"
#endif

/* "<version>+<commit 12>": PR channels reuse one version string, so the commit is what tells two
 * pr-N builds apart in a saved log. STATS carries it in 24 bytes (LP_FW_LEN). */
const char *firmware_id(void)
{
    static char id[64];
    if (!id[0]) snprintf(id, sizeof(id), "%s+%.12s", esp_app_get_description()->version, LETZTIMER_COMMIT);
    return id;
}

void firmware_print(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    char elf[65] = "";
    (void)esp_app_get_elf_sha256(elf, sizeof(elf));
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    lt_printf("FIRMWARE id=%s version=%s commit=%s elf_sha256=%s built=\"%s %s\" idf=%s engine=sdr_rx chip_rev=v%u.%u\n",
           firmware_id(), d->version, LETZTIMER_COMMIT, elf[0] ? elf : "unknown", d->date, d->time, d->idf_ver,
           (unsigned)(chip.revision / 100u), (unsigned)(chip.revision % 100u));
}
