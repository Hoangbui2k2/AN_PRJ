#include <string.h>
#include <stdio.h>
#include "topic.h"

/* Root prefix for all topics */
#define TOPIC_ROOT "irrigation"

/* Topic suffix strings */
#define SUFFIX_DATA   "data"
#define SUFFIX_ALARM  "alarm"
#define SUFFIX_STATUS "status"
#define SUFFIX_CMD    "cmd"
#define SUFFIX_CONFIG "config"

/**
 * @brief Get the suffix string for a given topic type
 */
static const char* topic_suffix(topic_type_t type)
{
    switch (type) {
        case TOPIC_NODE_DATA:   return SUFFIX_DATA;
        case TOPIC_NODE_ALARM:  return SUFFIX_ALARM;
        case TOPIC_NODE_STATUS: return SUFFIX_STATUS;
        case TOPIC_NODE_CMD:    return SUFFIX_CMD;
        case TOPIC_GW_STATUS:   return SUFFIX_STATUS;
        case TOPIC_GW_CONFIG:   return SUFFIX_CONFIG;
        case TOPIC_GW_WILL:     return SUFFIX_STATUS;
        default:                return SUFFIX_STATUS;
    }
}

void topic_build(char *buf, size_t buf_size,
                 const char *site, const char *gateway_id,
                 topic_type_t type, uint8_t node_id)
{
    if (!buf || buf_size == 0) return;

    const char *suffix = topic_suffix(type);

    switch (type) {
        case TOPIC_NODE_DATA:
        case TOPIC_NODE_ALARM:
        case TOPIC_NODE_STATUS:
        case TOPIC_NODE_CMD:
            snprintf(buf, buf_size, "%s/%s/%s/node_%02X/%s",
                     TOPIC_ROOT, site, gateway_id, node_id, suffix);
            break;

        case TOPIC_GW_STATUS:
        case TOPIC_GW_CONFIG:
        case TOPIC_GW_WILL:
            snprintf(buf, buf_size, "%s/%s/%s/%s",
                     TOPIC_ROOT, site, gateway_id, suffix);
            break;

        default:
            if (buf_size > 0) buf[0] = '\0';
            break;
    }
}

void topic_get_node_cmd_filter(char *buf, size_t buf_size,
                                const char *site, const char *gateway_id)
{
    if (!buf || buf_size == 0) return;

    snprintf(buf, buf_size, "%s/%s/%s/+/%s",
             TOPIC_ROOT, site, gateway_id, SUFFIX_CMD);
    /* Note: '+' wildcard matches "node_XX" where XX is the node ID in hex */
}

bool topic_parse(const char *topic, topic_info_t *info)
{
    if (!topic || !info) return false;

    memset(info, 0, sizeof(topic_info_t));

    /* Expected formats:
     *   irrigation/<site>/<gw_id>/node_<node_id>/<suffix>   (node topics, 4 slashes)
     *   irrigation/<site>/<gw_id>/<suffix>                  (gateway topics, 3 slashes)
     */

    /* Check root prefix */
    const size_t root_len = strlen(TOPIC_ROOT);
    if (strncmp(topic, TOPIC_ROOT, root_len) != 0)
        return false;

    const char *p = topic + root_len;
    if (*p != '/') return false;
    p++; /* Skip '/' */

    /* --- Token 1: site --- */
    const char *next = strchr(p, '/');
    if (!next) return false;
    {
        size_t len = (size_t)(next - p);
        if (len >= sizeof(info->site)) len = sizeof(info->site) - 1;
        memcpy(info->site, p, len);
        info->site[len] = '\0';
    }
    p = next + 1;

    /* --- Token 2: gateway_id --- */
    next = strchr(p, '/');
    if (!next) return false;
    {
        size_t len = (size_t)(next - p);
        if (len >= sizeof(info->gateway_id)) len = sizeof(info->gateway_id) - 1;
        memcpy(info->gateway_id, p, len);
        info->gateway_id[len] = '\0';
    }
    p = next + 1;

    /* --- Token 3: node_id OR suffix (for gateway-level topics) --- */
    next = strchr(p, '/');

    if (next) {
        /* 4 slashes → node topic: root/site/gw_id/node_id/suffix */
        /* Parse node_id from token 3 (format: "node_XX", e.g. "node_01", "node_A1") */
        {
            size_t len = (size_t)(next - p);
            const char *node_prefix = "node_";
            size_t prefix_len = strlen(node_prefix);

            /* Skip "node_" prefix if present */
            const char *id_start = p;
            if (len > prefix_len && strncmp(p, node_prefix, prefix_len) == 0) {
                id_start = p + prefix_len;
                len -= prefix_len;
            }

            if (len > 0) {
                char node_str[TOPIC_NODE_ID_STR_LEN] = {0};
                if (len >= sizeof(node_str)) len = sizeof(node_str) - 1;
                memcpy(node_str, id_start, len);
                node_str[len] = '\0';

                unsigned long val;
                if (sscanf(node_str, "%lx", &val) == 1) {
                    info->node_id = (uint8_t)(val & 0xFF);
                }
            }
        }

        /* Token 4: suffix */
        p = next + 1;
        if (strcmp(p, SUFFIX_DATA) == 0)
            info->type = TOPIC_NODE_DATA;
        else if (strcmp(p, SUFFIX_ALARM) == 0)
            info->type = TOPIC_NODE_ALARM;
        else if (strcmp(p, SUFFIX_STATUS) == 0)
            info->type = TOPIC_NODE_STATUS;
        else if (strcmp(p, SUFFIX_CMD) == 0)
            info->type = TOPIC_NODE_CMD;
        else
            return false; /* Unknown suffix */
    } else {
        /* 3 slashes → gateway topic: root/site/gw_id/suffix */
        if (strcmp(p, SUFFIX_STATUS) == 0)
            info->type = TOPIC_GW_STATUS;
        else if (strcmp(p, SUFFIX_CONFIG) == 0)
            info->type = TOPIC_GW_CONFIG;
        else
            return false; /* Unknown suffix */
    }

    info->valid = true;
    return true;
}
