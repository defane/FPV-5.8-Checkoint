/* FPV 5.8 Checkpoint detector (ESP32-C5): the sdr_rx engine sweeps R1-R8 in LO pairs, the
 * checkpoint glue detects passes and reports them over the UART link. No local output. */
#include <fcntl.h>
#include <stdio.h>
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "checkpoint.h"
#include "sdr_rx.h"

void app_main(void)
{
    const int64_t t0 = esp_timer_get_time();
    setvbuf(stdin, NULL, _IONBF, 0);                 /* USB output never blocks a task (as today) */
    setvbuf(stdout, NULL, _IONBF, 0);
    fcntl(fileno(stdout), F_SETFL, fcntl(fileno(stdout), F_GETFL, 0) | O_NONBLOCK);
    usb_serial_jtag_vfs_use_nonblocking();
    checkpoint_reserve();          /* scan task stack + TCB first: lowest heap, below the dump bank */
    checkpoint_console_start();    /* first: the download key '`' must work whatever follows */
    firmware_print();
    checkpoint_boot_stage("nvs");
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        (void)nvs_flash_erase();
        e = nvs_flash_init();
    }
    if (e != ESP_OK) { printf("BOOT failed step=nvs err=%s (console stays up)\n", esp_err_to_name(e)); return; }
    checkpoint_antenna_boot();     /* GPIO 26 before the PHY's boot calibration (IPEX variant) */
    checkpoint_boot_stage("radio");
    e = sdr_rx_radio_start();
    if (e != ESP_OK) { printf("BOOT failed step=radio err=%s (console stays up)\n", esp_err_to_name(e)); return; }
    printf("BOOT_TIMING radio_ms=%lld\n", (esp_timer_get_time() - t0) / 1000);
    checkpoint_start(t0);          /* prefs, detector, scan task (calibrates, then sweeps), UART link */
}
