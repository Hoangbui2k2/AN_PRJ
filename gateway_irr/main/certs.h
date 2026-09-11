#ifndef CERTS_H
#define CERTS_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NVS namespace and keys for the dedicated `certs` partition */
#define CERTS_NAMESPACE      "mqtt_certs"
#define CERTS_KEY_CA        "ca_cert"
#define CERTS_KEY_CLIENT    "client_cert"
#define CERTS_KEY_KEY       "client_key"

/**
 * @brief Open the dedicated `certs` NVS partition.
 *
 * Must be called once at startup before any certs_get_*() call.
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t certs_init(void);

/**
 * @brief Verify that all three certificates are present in the partition.
 *
 * @return true if CA + client cert + client key are all stored.
 */
bool certs_available(void);

/**
 * @brief Get the Amazon Root CA (PEM) from the certs partition.
 *
 * @return const char* NUL-terminated PEM string (caller must NOT free), or
 *         NULL if the blob is not present.
 */
const char *certs_get_ca(void);

/**
 * @brief Get the device client certificate (PEM) from the certs partition.
 */
const char *certs_get_client_cert(void);

/**
 * @brief Get the device private key (PEM) from the certs partition.
 */
const char *certs_get_client_key(void);

#ifdef __cplusplus
}
#endif

#endif /* CERTS_H */
