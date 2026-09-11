#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "certs.h"

static const char *TAG = "CERTS";

/* Label of the dedicated NVS partition that holds the X.509 PEMs
 * (see partitions.csv). */
#define CERTS_PARTITION "certs"

/* Cached cert buffers. Loaded once from the dedicated `certs` NVS partition at
 * startup so the MQTT layer can reference them via plain char pointers (the
 * esp-mqtt config stores const char* pointers, it does not copy the PEMs). */
static char *s_ca = NULL;
static char *s_client_cert = NULL;
static char *s_client_key = NULL;
static bool s_open = false;

/* Read a blob from NVS into a freshly malloc'd, NUL-terminated buffer.
 * Returns NULL if the key is missing. Caller must free(). */
static char *read_blob(nvs_handle_t handle, const char *key)
{
    size_t len = 0;
    esp_err_t ret = nvs_get_blob(handle, key, NULL, &len);
    if (ret != ESP_OK || len == 0) {
        return NULL;
    }
    /* +1 for NUL terminator (PEM strings must be NUL-terminated for mbedTLS). */
    char *buf = malloc(len + 1);
    if (!buf) {
        ESP_LOGE(TAG, "Out of memory loading cert '%s'", key);
        return NULL;
    }
    if (nvs_get_blob(handle, key, buf, &len) != ESP_OK) {
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    return buf;
}

esp_err_t certs_init(void)
{
    if (s_open) {
        return ESP_OK;
    }

    /* Initialize the dedicated `certs` NVS partition (separate from the main
     * `nvs` partition that holds gateway config). */
    esp_err_t ret = nvs_flash_init_partition(CERTS_PARTITION);
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NO_FREE_PAGES &&
        ret != ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "Failed to init '%s' partition: %s",
                 CERTS_PARTITION, esp_err_to_name(ret));
        return ret;
    }
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "'%s' partition needs erase, erasing...", CERTS_PARTITION);
        ESP_ERROR_CHECK(nvs_flash_erase_partition(CERTS_PARTITION));
        ret = nvs_flash_init_partition(CERTS_PARTITION);
        ESP_ERROR_CHECK(ret);
    }

    nvs_handle_t handle;
    ret = nvs_open_from_partition(CERTS_PARTITION, CERTS_NAMESPACE,
                                  NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open certs partition (namespace '%s'): %s",
                 CERTS_NAMESPACE, esp_err_to_name(ret));
        ESP_LOGE(TAG, "Flash certificates with tools/gen_certs_nvs.py before using AWS mode");
        return ret;
    }

    s_ca         = read_blob(handle, CERTS_KEY_CA);
    s_client_cert = read_blob(handle, CERTS_KEY_CLIENT);
    s_client_key  = read_blob(handle, CERTS_KEY_KEY);

    nvs_close(handle);
    s_open = true;

    ESP_LOGI(TAG, "Certs partition loaded: ca=%s client=%s key=%s",
             s_ca ? "OK" : "MISSING",
             s_client_cert ? "OK" : "MISSING",
             s_client_key ? "OK" : "MISSING");

    return ESP_OK;
}

bool certs_available(void)
{
    return s_ca != NULL && s_client_cert != NULL && s_client_key != NULL;
}

const char *certs_get_ca(void)        { return s_ca; }
const char *certs_get_client_cert(void) { return s_client_cert; }
const char *certs_get_client_key(void)  { return s_client_key; }
