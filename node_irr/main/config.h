#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_attr.h"
#include "sensors.h"   /* for sensor_data_t in delta API */

#ifdef __cplusplus
extern "C" {
#endif

#define NODE_ID 0x01
/* ──────────── Operation Modes ──────────── */
typedef enum {
    MODE_MANUAL    = 0,  /* Button-only pump control */
    MODE_SCHEDULE  = 1,  /* Time-based irrigation */
    MODE_THRESHOLD = 2,  /* Soil-moisture based irrigation */
} operation_mode_t;

/* ──────────── Alarm Codes ──────────── */
typedef enum {
    ALARM_NONE             = 0x00,
    ALARM_SENSOR_ERROR     = 0x01,
    ALARM_SOIL_OUT_RANGE   = 0x02,
    ALARM_RELAY_ERROR      = 0x03,
    ALARM_LOW_BATTERY      = 0x04,
    ALARM_GATEWAY_LOST     = 0x05,
} alarm_type_t;

/* ──────────── LoRa Packet Types ──────────── */
#define PKT_TYPE_DATA        0x01  /* Node -> Gateway: sensor data (8-byte fixed, legacy) */
#define PKT_TYPE_HEARTBEAT   0x02  /* Node -> Gateway: heartbeat (8-byte fixed, legacy) */
#define PKT_TYPE_ACK         0x03  /* Gateway -> Node / Node -> Gateway: ACK (8-byte fixed) */
#define PKT_TYPE_ALARM       0x04  /* Node -> Gateway: alarm (8-byte fixed) */
#define PKT_TYPE_CMD         0x05  /* Gateway -> Node: command (6-byte fixed) */
#define PKT_TYPE_DATA_COMPACT 0x06 /* Node -> Gateway: compact sensor data (variable-length) */

/* ──────────── Compact Packet Presence Bitmask (PKT_TYPE_DATA_COMPACT) ────────────
 *
 * Each bit indicates whether the corresponding field is present in the packet.
 * Fields are serialised in bit-order (LSB first):
 *
 *   bit0 = PRESENCE_TEMP     (1 byte, temperature offset+40, -40..+85°C)
 *   bit1 = PRESENCE_HUM      (1 byte, humidity 0-100%)
 *   bit2 = PRESENCE_SOIL     (1 byte, soil moisture 0-100%)
 *   bit3 = PRESENCE_BATTERY  (1 byte, battery 0-100% or 0xFF)
 *   bit4 = PRESENCE_FLAGS    (1 byte, FLAG_* bits — pump, threshold, gw, sensor)
 *   bit5-7 = reserved
 *
 * Packet size = 3 + popcount(presence) + 1(CRC)
 *   - node_id(1) + type(1) + presence(1) = 3 header bytes
 *   - N payload bytes (one per set presence bit, in order)
 *   - 1 CRC8 byte
 */
#define PRESENCE_TEMP      0x01
#define PRESENCE_HUM       0x02
#define PRESENCE_SOIL      0x04
#define PRESENCE_BATTERY   0x08
#define PRESENCE_FLAGS     0x10

/* ──────────── Data Packet Flag Bits ──────────── */
#define FLAG_PUMP_STATE           0x01  /* bit0: pump on/off */
#define FLAG_THRESHOLD_EXCEEDED   0x02  /* bit1: soil exceeded threshold */
#define FLAG_GATEWAY_LOST         0x04  /* bit2: gateway connectivity lost */
#define FLAG_SENSOR_OK            0x08  /* bit3: all sensors read successfully */

/* Helper: bitmask of all defined presence bits */
#define PRESENCE_ALL  (PRESENCE_TEMP | PRESENCE_HUM | PRESENCE_SOIL | PRESENCE_BATTERY | PRESENCE_FLAGS)

/* ──────────── Gateway Protocol Timing (new gateway) ──────────── */
#define DOWNLINK_WINDOW_MS      350    /* Post-uplink downlink listen window (spec: 200-500ms) */
#define GATEWAY_LOST_TIMEOUT_S  180    /* No downlink for this long → gatewayLostCount++ (spec example) */
#define GW_LOST_ALARM_THRESHOLD 3      /* gatewayLostCount >= 3 → GW_LOST flag + alarm 0x05 */

/* ──────────── Threshold Defaults ──────────── */
#define THRESHOLD_LOW_DEFAULT     30    /* Default lower threshold (%) */
#define THRESHOLD_HIGH_DEFAULT    70    /* Default upper threshold (%) */
#define HEARTBEAT_CYCLES          2     /* Send data heartbeat every N cycles */

/* ──────────── Delta Thresholds (change since last send) ──────────── */
#define TEMP_DELTA_THRESHOLD      20    /* 2.0°C, stored as °C × 10 */
#define HUM_DELTA_THRESHOLD        5    /* 5% RH */
#define SOIL_DELTA_THRESHOLD      10    /* 10% within absolute range */
#define BATTERY_DELTA_THRESHOLD   10    /* 10% (battery changes slowly) */

/* ──────────── Command Codes (downlink) ──────────── */
#define CMD_SET_INTERVAL         0x01
#define CMD_RELAY_ON             0x02
#define CMD_RELAY_OFF            0x03
#define CMD_SET_THRESHOLDS       0x04
#define CMD_SET_SCHEDULE         0x05
#define CMD_REQUEST_REPORT       0x06
#define CMD_TOGGLE_PUMP          0x07
#define CMD_SET_MODE             0x08
#define CMD_SYNC_TIME            0x09  /* Gateway keep-alive / empty ACK (no command) — node: reset gatewayLostCount, NO ACK back */
#define CMD_SET_DELTA_THRESHOLDS 0x0A  /* Set delta thresholds (param1=type, param2=value) */

/* Delta threshold types for CMD_SET_DELTA_THRESHOLDS */
#define DELTA_TYPE_TEMPERATURE   0    /* param2 = °C × 10 (default 20 = 2.0°C) */
#define DELTA_TYPE_HUMIDITY      1    /* param2 = % (default 5) */
#define DELTA_TYPE_SOIL          2    /* param2 = % (default 10) */
#define DELTA_TYPE_BATTERY       3    /* param2 = % (default 10) */

/* ──────────── LoRa 6-byte Command Packet ──────────── */
typedef struct __attribute__((packed)) {
    uint8_t dest;       /* 0xFF=broadcast or node ID */
    uint8_t type;       /* 0x05 = command */
    uint8_t cmd;        /* Command code */
    uint8_t param1;     /* Parameter 1 */
    uint8_t param2;     /* Parameter 2 */
    uint8_t crc;        /* CRC8 of bytes 0-4 */
} lora_cmd_packet_t;

/* ──────────── LoRa 8-byte Data Packet ──────────── */
typedef struct __attribute__((packed)) {
    uint8_t node_id;    /* Node identifier (0x01-0xFE) */
    uint8_t type;       /* Packet type */
    uint8_t flags;      /* bit0:pump, bit1:threshold_exceeded, bit2:gw_lost, bit3:sensor_ok, bit7-4:rsvd */
    uint8_t soil_moist; /* Soil moisture 0-100% */
    uint8_t temp;       /* Temperature: offset+40 (-40..+85 -> 0..125) */
    uint8_t humidity;   /* Humidity 0-100% */
    uint8_t battery;    /* Battery level 0-100% or 0xFF for external power */
    uint8_t crc;        /* CRC8 of bytes 0-6 */
} lora_data_packet_t;

/* ──────────── Configuration Structure (RTC Memory) ──────────── */
typedef struct __attribute__((packed)) {
    uint32_t magic;             /* Validity marker */
    uint16_t interval;          /* Deep sleep interval (seconds, default 300) */
    uint8_t  thresholdLow;      /* Soil moisture % to start irrigation */
    uint8_t  thresholdHigh;     /* Soil moisture % to stop irrigation */
    uint8_t  scheduleHour;      /* Schedule start hour (0-23) */
    uint8_t  scheduleMinute;    /* Schedule start minute (0-59) */
    uint16_t scheduleDuration;  /* Schedule run duration (seconds) */
    uint8_t  mode;              /* operation_mode_t */
    uint8_t  nodeId;            /* 0x01-0xFE */
    uint8_t  sensorErrorCount;  /* Consecutive sensor failures */
    uint8_t  gatewayLostCount;  /* Consecutive no-ACK attempts */
    bool     gatewayLost;       /* Gateway connection flag */
    bool     pumpState;         /* Current pump state */
    bool     thresholdExceeded; /* Set when soil moisture exceeds thresholds */
    uint16_t cyclesSinceSend;   /* Cycles since last data transmission */
    uint8_t  alarmCode;         /* Active alarm code (alarm_type_t) */
    uint32_t totalPumpCycles;   /* Total number of pump cycles */
    uint32_t lastScheduleTime;  /* Last synced schedule epoch time */
    bool     pumpBySchedule;    /* Flag indicating pump was started by schedule */
    uint16_t normalInterval;    /* Normal deep sleep interval to restore */
    uint16_t lastWateringDay;   /* Track day of year to ensure only once per day watering */

    /* Last-sent values for delta-threshold detection */
    int8_t   lastSentTemp;      /* Last temperature × 10 sent (INT8_MIN = never sent) */
    uint8_t  lastSentHumidity;  /* Last humidity % sent */
    uint8_t  lastSentSoil;      /* Last soil moisture % sent */
    uint8_t  lastSentBattery;   /* Last battery % sent */
    uint8_t  lastSentFlags;     /* Last FLAG_* bits sent */

    /* Configurable delta thresholds (overridable via LoRa CMD 0x0A) */
    uint8_t  deltaTemp;         /* °C × 10 (default TEMP_DELTA_THRESHOLD) */
    uint8_t  deltaHumidity;     /* % (default HUM_DELTA_THRESHOLD) */
    uint8_t  deltaSoil;         /* % (default SOIL_DELTA_THRESHOLD) */
    uint8_t  deltaBattery;      /* % (default BATTERY_DELTA_THRESHOLD) */
} app_config_t;

#define CONFIG_MAGIC 0x49525247  /* "IRRG" */

/* ──────────── Public API ──────────── */

void config_init_default(void);
bool config_load(void);
void config_save(void);
app_config_t *config_get(void);

void config_set_alarm(uint8_t alarm);
void config_clear_alarm(void);
void config_set_pump_state(bool on);
void config_toggle_pump(void);
void config_set_mode(operation_mode_t mode);
void config_set_sleep_time(uint16_t seconds);
void config_set_thresholds(uint8_t low, uint8_t high);
void config_set_schedule(uint8_t hour, uint8_t minute);
void config_reset_gw_lost(void);
void config_increment_gw_lost(void);
void config_reset_sensor_errors(void);
void config_increment_sensor_errors(void);

/* ──────────── Threshold / Heartbeat / Delta API ──────────── */
void config_set_threshold_exceeded(bool exceeded);
bool node_check_threshold(uint8_t soil_moisture_pct);
bool node_check_delta(const sensor_data_t *data);
uint8_t node_check_delta_fields(const sensor_data_t *data);
void node_store_last_sent(const sensor_data_t *data);
void node_store_last_sent_compact(const sensor_data_t *data, uint8_t presence);
bool is_heartbeat_time(void);
void increment_cycle_counter(void);
void reset_cycle_counter(void);

/* ──────────── Configurable Delta Thresholds ──────────── */
uint8_t config_get_delta_temp(void);
uint8_t config_get_delta_humidity(void);
uint8_t config_get_delta_soil(void);
uint8_t config_get_delta_battery(void);
void config_set_delta_temp(uint8_t val);
void config_set_delta_humidity(uint8_t val);
void config_set_delta_soil(uint8_t val);
void config_set_delta_battery(uint8_t val);

/* ──────────── NVS Threshold Persistence ──────────── */
void save_threshold_to_nvs(void);
void load_threshold_from_nvs(void);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */
