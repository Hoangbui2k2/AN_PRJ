#ifndef TOPIC_H
#define TOPIC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum buffer size for a full topic string */
#define TOPIC_MAX_LEN      128

/* Maximum lengths for identity fields */
#define TOPIC_SITE_LEN     32
#define TOPIC_GW_ID_LEN    32
#define TOPIC_NODE_ID_STR_LEN 8

/**
 * @brief Types of MQTT topics used in the system
 */
typedef enum {
    TOPIC_NODE_DATA,     /**< irrigation/<site>/<gw_id>/node_<node_id>/data */
    TOPIC_NODE_ALARM,    /**< irrigation/<site>/<gw_id>/node_<node_id>/alarm */
    TOPIC_NODE_STATUS,   /**< irrigation/<site>/<gw_id>/node_<node_id>/status */
    TOPIC_NODE_CMD,      /**< irrigation/<site>/<gw_id>/node_<node_id>/cmd */
    TOPIC_NODE_BASELINE, /**< irrigation/<site>/<gw_id>/node_<node_id>/baseline */
    TOPIC_GW_STATUS,     /**< irrigation/<site>/<gw_id>/status */
    TOPIC_GW_CONFIG,     /**< irrigation/<site>/<gw_id>/config */
    TOPIC_GW_WILL,       /**< irrigation/<site>/<gw_id>/status (last will) */
} topic_type_t;

/**
 * @brief Parsed topic information
 */
typedef struct {
    char site[TOPIC_SITE_LEN];         /**< Site name */
    char gateway_id[TOPIC_GW_ID_LEN];  /**< Gateway identifier */
    uint8_t node_id;                   /**< Node ID (0 if not a node topic) */
    topic_type_t type;                 /**< Detected topic type */
    bool valid;                        /**< Whether parsing succeeded */
} topic_info_t;

/**
 * @brief Build a topic string from components
 *
 * Constructs a topic string in the format:
 *   irrigation/<site>/<gateway_id>/node_<node_id>/<suffix>
 * or for gateway-level topics:
 *   irrigation/<site>/<gateway_id>/<suffix>
 *
 * @param buf       Output buffer for the topic string
 * @param buf_size  Size of output buffer
 * @param site      Site name (e.g., "factory_1")
 * @param gateway_id Gateway identifier (e.g., "gw_01")
 * @param type      Type of topic to build
 * @param node_id   Node ID (ignored for gateway-level topics)
 */
void topic_build(char *buf, size_t buf_size,
                 const char *site, const char *gateway_id,
                 topic_type_t type, uint8_t node_id);

/**
 * @brief Parse an incoming topic string into its components
 *
 * Expected format: irrigation/<site>/<gateway_id>/node_<node_id>/<suffix>
 *                  irrigation/<site>/<gateway_id>/<suffix>
 *
 * @param topic  Incoming topic string
 * @param info   Output structure with parsed fields
 * @return true if parsing succeeded, false otherwise
 */
bool topic_parse(const char *topic, topic_info_t *info);

/**
 * @brief Get the subscribe filter for all node command topics
 *
 * Returns a wildcard filter: irrigation/<site>/<gateway_id>/+/cmd
 * The '+' wildcard matches "node_XX" for any node under this gateway.
 * Used to subscribe to commands for all nodes under this gateway.
 *
 * @param buf       Output buffer
 * @param buf_size  Size of buffer
 * @param site      Site name
 * @param gateway_id Gateway identifier
 */
void topic_get_node_cmd_filter(char *buf, size_t buf_size,
                                const char *site, const char *gateway_id);

#ifdef __cplusplus
}
#endif

#endif /* TOPIC_H */
