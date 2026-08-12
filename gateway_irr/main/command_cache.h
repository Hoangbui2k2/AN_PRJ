#ifndef COMMAND_CACHE_H
#define COMMAND_CACHE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_CACHED_COMMANDS 20
#define MAX_RETRY           5

/* Exponential backoff base delay in milliseconds */
#define RETRY_BASE_DELAY_MS 1000

/* How long to wait for a node ACK after sending before retrying */
#define ACK_TIMEOUT_MS      3000

/**
 * @brief Cached command entry structure
 */
typedef struct {
    bool     pending;       /* Whether this slot is occupied */
    uint8_t  node_id;       /* Target node ID */
    uint8_t  cmd[6];        /* Raw 6-byte command packet */
    uint8_t  retry_count;   /* Current retry attempt number */
    uint64_t next_retry_ms; /* Timestamp for next retry attempt */
    uint64_t last_sent_ms;  /* Timestamp of last send (0 = never sent) */
    bool     waiting_ack;   /* True while waiting for node ACK after a send */
    bool     acked;         /* Whether node has acknowledged this command */
} cached_command_t;

/**
 * @brief Initialize the command cache
 */
void command_cache_init(void);

/**
 * @brief Cache a command for a node
 * 
 * Stores the command in the first available slot. If cache is full,
 * replaces the oldest pending entry (FIFO eviction).
 * 
 * @param node_id Target node ID
 * @param cmd Pointer to 6-byte command packet
 */
void command_cache_add(uint8_t node_id, const uint8_t *cmd);

/**
 * @brief Get the number of pending commands for a specific node
 * 
 * @param node_id Node ID to check
 * @return uint8_t Number of pending commands
 */
uint8_t command_cache_count_for_node(uint8_t node_id);

/**
 * @brief Get the total number of pending commands across all nodes
 * 
 * @return uint8_t Total pending count
 */
uint8_t command_cache_total_pending(void);

/**
 * @brief Get the next pending command for a node (FIFO order)
 * 
 * Retrieves the oldest pending command for the given node that is NOT
 * currently waiting for an ACK. Does NOT remove it from the cache.
 * 
 * @param node_id Node ID
 * @return cached_command_t* Pointer to command entry, or NULL if none pending
 */
cached_command_t* command_cache_get_next(uint8_t node_id);

/**
 * @brief Get the in-flight command for a node (sent but not yet ACKed)
 * 
 * Retrieves the command that was most recently sent to the node and is
 * still waiting for an ACK. At most one command per node is in-flight
 * at a time.
 * 
 * @param node_id Node ID
 * @return cached_command_t* Pointer to the in-flight command, or NULL
 */
cached_command_t* command_cache_get_inflight(uint8_t node_id);

/**
 * @brief Mark a command as sent and start waiting for its ACK
 * 
 * Records the current time in last_sent_ms and sets waiting_ack.
 * The command will not be re-sent until either an ACK arrives or
 * ACK_TIMEOUT_MS elapses.
 * 
 * @param cmd Pointer to the command entry
 */
void command_cache_mark_sent(cached_command_t *cmd);

/**
 * @brief Mark a command as acknowledged and remove from cache
 * 
 * @param node_id Node ID
 * @param cmd Pointer to the acknowledged command packet
 * @return true if command was found and removed
 */
bool command_cache_ack(uint8_t node_id, const uint8_t *cmd);

/**
 * @brief Remove all pending commands for a node
 * 
 * @param node_id Node ID
 */
void command_cache_clear_for_node(uint8_t node_id);

/**
 * @brief Retry logic: process ACK timeouts and return the next command to send
 * 
 * Called periodically. First, any command that is waiting for an ACK
 * past ACK_TIMEOUT_MS is advanced to its next retry (or dropped when
 * retry_count >= MAX_RETRY). Then returns the earliest command that is
 * due for (re)transmission.
 * 
 * @return cached_command_t* Pointer to command ready for retry, or NULL
 */
cached_command_t* command_cache_get_retry_ready(void);

/**
 * @brief Advance retry bookkeeping for commands whose ACK timed out, and drop
 *        commands that exceeded MAX_RETRY.
 *
 * Bookkeeping-only: it NEVER transmits. Actual (re)transmission happens in the
 * uplink path (send_cached_commands_for_node via ack_or_flush_node), which is
 * the only moment we know the node is truly awake. After an ACK timeout a
 * command becomes "ready" again so the next uplink re-flushes it.
 */
void command_cache_process_timeouts(void);

/**
 * @brief Advance retry state for a command (increment count, set next retry time)
 *
 * @param cmd Pointer to the command entry
 */
void command_cache_advance_retry(cached_command_t *cmd);

/**
 * @brief Remove a specific command from cache
 * 
 * @param cmd Pointer to the command entry to remove
 */
void command_cache_remove(cached_command_t *cmd);

/**
 * @brief Print cache status to log
 */
void command_cache_print(void);

#ifdef __cplusplus
}
#endif

#endif /* COMMAND_CACHE_H */
