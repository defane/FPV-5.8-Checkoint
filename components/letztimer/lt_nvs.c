/* NVS blobs of the detector, namespace "letztimer". After C5VRX-5 settings.c blob_load()/
 * blob_store() (Twotoz and the C5VRX contributors, https://github.com/Twotoz/C5VRX, 6bc9e4b,
 * GPL-3.0-only). Nothing here opens C5VRX's namespace "c5vrx5": a reflash of a C5VRX build finds
 * its settings there intact. */
#include <stdio.h>
#include "lt_internal.h"
#include "lt_log.h"
#include "nvs.h"

#define NAMESPACE "letztimer"

bool lt_blob_load(const char *key, void *data, size_t size)
{
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;   /* none yet: nothing saved */
    size_t length = size;
    const esp_err_t err = nvs_get_blob(h, key, data, &length);
    nvs_close(h);
    return err == ESP_OK && length == size;
}

bool lt_blob_store(const char *key, const void *data, size_t size)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, key, data, size);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) lt_printf("NVS store key=%s err=%s\n", key, esp_err_to_name(err));
    return err == ESP_OK;
}
