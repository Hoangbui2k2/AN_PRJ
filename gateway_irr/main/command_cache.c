#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "command_cache.h"

static const char *TAG = "CMD_CACHE";

static cached_command_t s_cache[MAX_CACHED_COMMANDS];

void command_cache_init(void)
{
    memset(s_cache, 0, sizeof(s_cache));
    ESP_LOGI(TAG, "Command cache initialized (max %d commands)", MAX_CACHED_COMMANDS);
}

void command_cache_add(uint8_t node_id, const uint8_t *cmd)
{
    /* First, check if there's already a pending command for this node that matches */
    for (int i = 0; i < MAX_CACHED_COMMANDS; i++) {
        if (s_cache[i].pending && s_cache[i].node_id == node_id &&
            memcmp(s_cache[i].cmd, cmd, 6) == 0) {
            ESP_LOGD(TAG, "Duplicate command for node 0x%02X already cached, skipping", node_id);
            return;
        }
    }

    /* Find first empty slot */
    for (int i = 0; i < MAX_CACHED_COMMANDS; i++) {
        if (!s_cache[i].pending) {
            s_cache[i].pending = true;
            s_cache[i].node_id = node_id;
            memcpy(s_cache[i].cmd, cmd, 6);
            s_cache[i].retry_count = 0;
            s_cache[i].next_retry_ms = 0; /* Ready immediately */
            s_cache[i].last_sent_ms = 0;
            s_cache[i].waiting_ack = false;
            s_cache[i].acked = false;
            ESP_LOGI(TAG, "Cached command for node 0x%02X (slot %d, total pending: %d)",
                     node_id, i, command_cache_total_pending());
            return;
        }
    }

    /* Cache full: FIFO eviction - replace the oldest pending entry */
    int oldest_idx = -1;
    uint64_t oldest_time = UINT64_MAX;

    for (int i = 0; i < MAX_CACHED_COMMANDS; i++) {
        if (s_cache[i].pending && s_cache[i].next_retry_ms < oldest_time) {
            oldest_time = s_cache[i].next_retry_ms;
            oldest_idx = i;
        }
    }

    if (oldest_idx >= 0) {
        ESP_LOGW(TAG, "Command cache full, evicting oldest command for node 0x%02X",
                 s_cache[oldest_idx].node_id);
        s_cache[oldest_idx].pending = true;
        s_cache[oldest_idx].node_id = node_id;
        memcpy(s_cache[oldest_idx].cmd, cmd, 6);
        s_cache[oldest_idx].retry_count = 0;
        s_cache[oldest_idx].next_retry_ms = 0;
        s_cache[oldest_idx].last_sent_ms = 0;
        s_cache[oldest_idx].waiting_ack = false;
        s_cache[oldest_idx].acked = false;
        ESP_LOGI(TAG, "Cached command for node 0x%02X (slot %d, evicted oldest)", node_id, oldest_idx);
    } else {
        ESP_LOGE(TAG, "Command cache full and no entries to evict");
    }
}

uint8_t command_cache_count_for_node(uint8_t node_id)
{
    uint8_t count = 0;
    for (int i = 0; i < MAX_CACHED_COMMANDS; i++) {
        if (s_cache[i].pending && s_cache[i].node_id == node_id) {
            count++;
        }
    }
    return count;
}

uint8_t command_cache_total_pending(void)
{
    uint8_t count = 0;
    for (int i = 0; i < MAX_CACHED_COMMANDS; i++) {
        if (s_cache[i].pending) {
            count++;
        }
    }
    return count;
}

cached_command_t* command_cache_get_next(uint8_t node_id)
{
    cached_command_t *first = NULL;

    /* FIFO: find the oldest pending command for this node.
     * Skip commands that are still waiting for an ACK so that only one
     * command per node is in-flight at a time. */
    for (int i = 0; i < MAX_CACHED_COMMANDS; i++) {
        if (s_cache[i].pending && s_cache[i].node_id == node_id &&
            !s_cache[i].waiting_ack) {
            if (!first || s_cache[i].next_retry_ms < first->next_retry_ms) {
                first = &s_cache[i];
            }
        }
    }

    return first;
}

cached_command_t* command_cache_get_inflight(uint8_t node_id)
{
    for (int i = 0; i < MAX_CACHED_COMMANDS; i++) {
        if (s_cache[i].pending && !s_cache[i].acked &&
            s_cache[i].node_id == node_id && s_cache[i].waiting_ack) {
            return &s_cache[i];
        }
    }
    return NULL;
}

void command_cache_mark_sent(cached_command_t *cmd)
{
    cmd->last_sent_ms = esp_timer_get_time() / 1000;
    cmd->waiting_ack = true;
    ESP_LOGD(TAG, "Command sent to node 0x%02X, waiting for ACK (timeout %d ms)",
             cmd->node_id, ACK_TIMEOUT_MS);
}

bool command_cache_ack(uint8_t node_id, const uint8_t *cmd)
{
    for (int i = 0; i < MAX_CACHED_COMMANDS; i++) {
        if (s_cache[i].pending && s_cache[i].node_id == node_id &&
            memcmp(s_cache[i].cmd, cmd, 6) == 0) {
            s_cache[i].pending = false;
            s_cache[i].acked = true;
            s_cache[i].waiting_ack = false;
            s_cache[i].last_sent_ms = 0;
            ESP_LOGI(TAG, "Command ACKed for node 0x%02X (slot %d, %d pending remain)",
                     node_id, i, command_cache_total_pending());
            return true;
        }
    }
    return false;
}

void command_cache_clear_for_node(uint8_t node_id)
{
    uint8_t cleared = 0;
    for (int i = 0; i < MAX_CACHED_COMMANDS; i++) {
        if (s_cache[i].pending && s_cache[i].node_id == node_id) {
            s_cache[i].pending = false;
            cleared++;
        }
    }
    if (cleared > 0) {
        ESP_LOGI(TAG, "Cleared %d cached command(s) for node 0x%02X", cleared, node_id);
    }
}

cached_command_t* command_cache_get_retry_ready(void)
{
    uint64_t now_ms = esp_timer_get_time() / 1000;
    cached_command_t *first = NULL;
    uint64_t first_retry_ms = UINT64_MAX;

    for (int i = 0; i < MAX_CACHED_COMMANDS; i++) {
        cached_command_t *c = &s_cache[i];
        if (!c->pending || c->acked) {
            continue;
        }

        /* If this command is waiting for an ACK, check the timeout */
        if (c->waiting_ack) {
            if (c->last_sent_ms != 0 &&
                (now_ms - c->last_sent_ms) >= ACK_TIMEOUT_MS) {
                ESP_LOGW(TAG, "ACK timeout for node 0x%02X (%llu ms since send), retrying",
                         c->node_id, (unsigned long long)(now_ms - c->last_sent_ms));
                command_cache_advance_retry(c);
                if (c->retry_count >= MAX_RETRY) {
                    ESP_LOGW(TAG, "Command for node 0x%02X exceeded max retries (%d), dropping",
                             c->node_id, MAX_RETRY);
                    c->pending = false;
                }
            }
            /* Still within the ACK window — never resend it here */
            continue;
        }

        /* Not waiting for ACK: check if it is due for (re)transmission */
        if (now_ms >= c->next_retry_ms && c->next_retry_ms < first_retry_ms) {
            first_retry_ms = c->next_retry_ms;
            first = c;
        }
    }

    return first;
}

/**
 * @brief Advance retry bookkeeping for commands whose ACK timed out, and drop
 *        commands that exceeded MAX_RETRY.
 *
 * Bookkeeping-only: it never transmits. Actual (re)transmission is driven by
 * the uplink path (send_cached_commands_for_node via ack_or_flush_node), which
 * is the only time we know the node is awake. This keeps a command that was
 * sent but not ACKed from being forgotten: after ACK_TIMEOUT_MS it becomes
 * "ready" again (waiting_ack cleared, retry_count++), so the next uplink from
 * the node re-flushes it.
 */
void command_cache_process_timeouts(void)
{
    uint64_t now_ms = esp_timer_get_time() / 1000;

    for (int i = 0; i < MAX_CACHED_COMMANDS; i++) {
        cached_command_t *c = &s_cache[i];
        if (!c->pending || c->acked) {
            continue;
        }

        if (c->waiting_ack && c->last_sent_ms != 0 &&
            (now_ms - c->last_sent_ms) >= ACK_TIMEOUT_MS) {
            ESP_LOGW(TAG, "ACK timeout for node 0x%02X (%llu ms since send)",
                     c->node_id, (unsigned long long)(now_ms - c->last_sent_ms));
            command_cache_advance_retry(c);
            if (c->retry_count >= MAX_RETRY) {
                ESP_LOGW(TAG, "Command for node 0x%02X exceeded max retries (%d), dropping",
                         c->node_id, MAX_RETRY);
                c->pending = false;
            }
        }
    }
}

void command_cache_advance_retry(cached_command_t *cmd)
{
    cmd->retry_count++;
    cmd->waiting_ack = false;
    cmd->last_sent_ms = 0;
    
    /* Exponential backoff: 1s, 2s, 4s, 8s, 16s */
    uint32_t delay_ms = RETRY_BASE_DELAY_MS << (cmd->retry_count - 1);
    if (delay_ms > 16000) delay_ms = 16000;

    cmd->next_retry_ms = (esp_timer_get_time() / 1000) + delay_ms;

    ESP_LOGD(TAG, "Advance retry for node 0x%02X: attempt %d/%d, next retry in %lu ms",
             cmd->node_id, cmd->retry_count, MAX_RETRY, (unsigned long)delay_ms);
}

void command_cache_remove(cached_command_t *cmd)
{
    cmd->pending = false;
    cmd->acked = true;
    cmd->waiting_ack = false;
    cmd->last_sent_ms = 0;
}

void command_cache_print(void)
{
    uint8_t total = command_cache_total_pending();
    ESP_LOGI(TAG, "===== Command Cache (%d pending) =====", total);
    for (int i = 0; i < MAX_CACHED_COMMANDS; i++) {
        if (s_cache[i].pending) {
            ESP_LOGI(TAG, "Slot %d: Node=0x%02X Cmd=%02X %02X %02X %02X %02X %02X "
                     "Retry=%d/%d %s",
                     i, s_cache[i].node_id,
                     s_cache[i].cmd[0], s_cache[i].cmd[1], s_cache[i].cmd[2],
                     s_cache[i].cmd[3], s_cache[i].cmd[4], s_cache[i].cmd[5],
                     s_cache[i].retry_count, MAX_RETRY,
                     s_cache[i].waiting_ack ? "[waiting ACK]" : "[ready]");
        }
    }
    ESP_LOGI(TAG, "================================");
}
