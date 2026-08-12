#ifndef NODE_MANAGER_H
#define NODE_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_NODES       10
#define HEARTBEAT_INTERVAL_MS 60000  /* 60 seconds between node heartbeats (legacy/default) */

/* ── Offline timeout, derived from the node's real heartbeat period ──
 *
 * The node sends a heartbeat every (NODE_HB_CYCLES + 1) wake cycles, where
 * NODE_HB_CYCLES matches HEARTBEAT_CYCLES in the node firmware (= 5). The
 * gateway computes offline timeout = 2 × heartbeat period, i.e.
 *     timeout = sleep_interval_s × (NODE_HB_CYCLES+1) × NODE_TIMEOUT_HB_MULT
 * so a slow/heartbeat-thin node is not marked offline before it can report.
 */
#define NODE_HB_CYCLES       5   /* node HEARTBEAT_CYCLES (wake cycles per heartbeat) */
#define NODE_TIMEOUT_HB_MULT 2   /* offline after 2 missed heartbeat periods */
#define NODE_TIMEOUT_MIN_MS  60000 /* floor: never offline sooner than 60s */

/* Default report interval (seconds) — used by set_interval default */
#define REPORT_INTERVAL_DEFAULT 10

/* Default threshold values */
#define THRESHOLD_LOW_DEFAULT   30   /* Default lower threshold (%) */
#define THRESHOLD_HIGH_DEFAULT  70   /* Default upper threshold (%) */

/* Default delta threshold values (see DELTA_TYPE_* in lora_uart.h) */
#define DELTA_TEMP_DEFAULT      20   /* 2.0°C  (value = °C × 10) */
#define DELTA_HUM_DEFAULT        5   /* 5% */
#define DELTA_SOIL_DEFAULT      10   /* 10% */
#define DELTA_BATTERY_DEFAULT   10   /* 10% */

/**
 * @brief Node entry structure
 *
 * Stores all runtime state for a managed node.
 */
typedef struct {
    uint8_t  id;             /* Node identifier */
    bool     online;         /* Online status */
    uint64_t last_seen_ms;   /* Last packet timestamp (milliseconds) */
    uint8_t  soil;           /* Last soil moisture reading */
    int8_t   temp;           /* Last temperature reading */
    uint8_t  hum;            /* Last humidity reading */
    uint8_t  battery;        /* Last battery level */
    uint8_t  pump_state;     /* Pump state (0=off, 1=on) */
    uint8_t  flags;          /* Last flags field */

    /* ── Absolute threshold (soil) ── */
    uint8_t  threshold_low;     /* Lower threshold (soil % below this → trigger) */
    uint8_t  threshold_high;    /* Upper threshold (soil % above this → trigger) */
    bool     threshold_exceeded;/* true when soil is outside threshold band */

    /* ── Delta threshold (change-based reporting) ── */
    uint8_t  delta_temp;        /* Temperature delta (×10, e.g. 20 = 2.0°C) */
    uint8_t  delta_hum;         /* Humidity delta (%) */
    uint8_t  delta_soil;        /* Soil moisture delta (%) */
    uint8_t  delta_battery;     /* Battery delta (%) */

    /* ── Last reported values (for delta comparison) ── */
    int8_t   last_reported_temp;
    uint8_t  last_reported_hum;
    uint8_t  last_reported_soil;
    uint8_t  last_reported_battery;

    /* ── Intervals (seconds, defaults from config) ── */
    uint16_t report_interval;      /* Sensor report interval (seconds) */
    uint16_t heartbeat_interval;   /* Heartbeat interval (seconds, legacy default) */
    uint16_t sleep_interval_s;     /* Deep-sleep period per wake cycle (sec).
                                    * Source for the per-node offline timeout. */

    /* ── Schedule & mode (defaults from config) ── */
    uint8_t  schedule_hour;        /* Default schedule hour (0-23) */
    uint8_t  schedule_minute;      /* Default schedule minute (0-59) */
    uint8_t  mode;                 /* Default operating mode */
} node_entry_t;

/**
 * @brief Initialize the node manager
 */
void node_manager_init(void);

/**
 * @brief Find or create a node entry by ID
 * 
 * If the node exists, returns pointer to existing entry.
 * If not and there is space, creates a new entry.
 * 
 * @param id Node ID to find or create
 * @return node_entry_t* Pointer to node entry, or NULL if full
 */
node_entry_t* node_find_or_create(uint8_t id);

/**
 * @brief Register a node with explicit default settings (from config)
 *
 * Creates the node entry (or updates an existing one) with the given
 * default thresholds, delta thresholds and intervals. These values are
 * used as fallbacks when a server command omits parameters.
 *
 * @param id Node ID
 * @param threshold_low Lower soil threshold (%)
 * @param threshold_high Upper soil threshold (%)
 * @param delta_temp Delta temperature (×10)
 * @param delta_hum Delta humidity (%)
 * @param delta_soil Delta soil moisture (%)
 * @param delta_battery Delta battery (%)
 * @param report_interval Report interval (seconds)
 * @param heartbeat_interval Heartbeat interval (seconds)
 * @param schedule_hour Default schedule hour (0-23)
 * @param schedule_minute Default schedule minute (0-59)
 * @param mode Default operating mode
 * @return true on success
 */
bool node_register(uint8_t id, uint8_t threshold_low, uint8_t threshold_high,
                   uint8_t delta_temp, uint8_t delta_hum, uint8_t delta_soil,
                   uint8_t delta_battery, uint16_t report_interval,
                   uint16_t heartbeat_interval, uint8_t schedule_hour,
                   uint8_t schedule_minute, uint8_t mode);

/**
 * @brief Find a node by ID (read-only lookup)
 * 
 * @param id Node ID to find
 * @return node_entry_t* Pointer to node entry, or NULL if not found
 */
node_entry_t* node_find(uint8_t id);

/**
 * @brief Update a node's sensor data
 * 
 * Updates the node entry with new sensor readings and marks it online.
 * 
 * @param id Node ID
 * @param soil Soil moisture
 * @param temp Temperature
 * @param hum Humidity
 * @param battery Battery level
 * @param pump_state Pump state
 * @param flags Flags field
 */
void node_update_data(uint8_t id, uint8_t soil, int8_t temp, uint8_t hum,
                      uint8_t battery, uint8_t pump_state, uint8_t flags);

/**
 * @brief Set threshold values for a node
 *
 * Sets the low/high soil moisture thresholds that determine
 * whether sensor data is considered "exceeding" and should
 * trigger an alarm.
 *
 * @param id Node ID
 * @param low Lower threshold (0-100)
 * @param high Upper threshold (0-100)
 */
void node_set_threshold(uint8_t id, uint8_t low, uint8_t high);

/**
 * @brief Check if a sensor reading exceeds the node's thresholds
 *
 * Returns true and sets threshold_exceeded flag if the soil moisture
 * reading is outside the node's configured threshold band.
 *
 * @param id Node ID
 * @param soil Current soil moisture reading
 * @return true if soil < low or soil > high
 */
bool node_check_threshold(uint8_t id, uint8_t soil);

/**
 * @brief Get the current threshold_exceeded flag for a node
 *
 * @param id Node ID
 * @return true if threshold was exceeded at last check
 */
bool node_is_threshold_exceeded(uint8_t id);

/**
 * @brief Set a delta threshold for a node
 *
 * Configures the minimum change required before a node will send
 * an unsolicited data packet for a specific sensor type.
 *
 * @param id Node ID
 * @param type One of DELTA_TYPE_TEMPERATURE / _HUMIDITY / _SOIL / _BATTERY
 * @param value Delta threshold value
 * @return true if type was valid and value was set
 */
bool node_set_delta_threshold(uint8_t id, uint8_t type, uint8_t value);

/**
 * @brief Set the node's deep-sleep period (seconds per wake cycle)
 *
 * Updates node_entry.sleep_interval_s which drives the per-node offline
 * timeout (see NODE_HB_CYCLES / NODE_TIMEOUT_HB_MULT). Called when the
 * server sends set_interval so the timeout follows the node.
 *
 * @param id Node ID
 * @param seconds Sleep period in seconds (clamped 5..3600)
 */
void node_set_sleep_interval(uint8_t id, uint16_t seconds);

/**
 * @brief Get the delta threshold value for a given type
 *
 * @param id Node ID
 * @param type One of DELTA_TYPE_*
 * @return uint8_t Current delta value, or 0 on error
 */
uint8_t node_get_delta_threshold(uint8_t id, uint8_t type);

/**
 * @brief Update last_reported_* values to current readings
 *
 * Called after a data packet is published to MQTT, so subsequent
 * delta comparisons use the freshly-reported values as baseline.
 *
 * @param id Node ID
 */
void node_update_last_reported(uint8_t id);

/**
 * @brief Check all nodes for timeout and mark offline if needed
 * 
 * Iterates through all registered nodes and marks any that have exceeded
 * NODE_TIMEOUT_MS since last_seen as offline.
 * Returns a bitmask of nodes that changed state.
 * 
 * @return uint16_t Bitmask of nodes that transitioned from online to offline
 */
uint16_t node_check_timeouts(void);

/**
 * @brief Mark a node as online (without updating sensor data)
 * 
 * @param id Node ID
 */
void node_mark_online(uint8_t id);

/**
 * @brief Mark a node as offline
 * 
 * @param id Node ID
 */
void node_mark_offline(uint8_t id);

/**
 * @brief Get the current node count
 * 
 * @return uint8_t Number of registered nodes
 */
uint8_t node_get_count(void);

/**
 * @brief Get pointer to all nodes array
 * 
 * @return node_entry_t* Pointer to nodes array
 */
node_entry_t* node_get_all(void);

/**
 * @brief Get current time in milliseconds
 * 
 * @return uint64_t Current time in milliseconds
 */
uint64_t node_get_time_ms(void);

/**
 * @brief Dump all node statuses to log
 */
void node_print_all(void);

#ifdef __cplusplus
}
#endif

#endif /* NODE_MANAGER_H */
