#ifndef BASELINE_H
#define BASELINE_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "sensors.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * One committed baseline series: up to BASELINE_MAX_POINTS (t, y) pairs.
 *   - t[] is ascending slot index (0..95)
 *   - y[] is the 1-byte value in the series' native encoding:
 *       temp : °C + 40   (0..125)
 *       hum  : 0..100 %
 *       soil : 0..100 %
 */
typedef struct {
    uint8_t version;                    /* 0 = series absent / invalid */
    uint8_t count;                      /* number of points (0..BASELINE_MAX_POINTS) */
    uint8_t t[BASELINE_MAX_POINTS];
    uint8_t y[BASELINE_MAX_POINTS];
} baseline_series_t;

/* ──────────── Lifecycle ──────────── */

/**
 * @brief Load committed baselines from NVS into RAM.
 *        Call once after nvs_flash_init(). Safe to call every boot.
 * @return true if at least one series is valid
 */
bool baseline_init(void);

/** @return true if the series has a stored baseline */
bool baseline_series_valid(uint8_t series);

/** @return stored version of a series (0 = none) */
uint8_t baseline_version(uint8_t series);

/** @return bitmask of series that currently have a valid baseline */
uint8_t baseline_valid_mask(void);

/* ──────────── Reassembly (type 0x07 chunks) ──────────── */

/**
 * @brief Start (or restart) a reassembly session for the given series mask.
 * @param series_mask Bit0=temp, bit1=hum, bit2=soil
 * @param version     Expected version (informational; chunks carry their own)
 */
void baseline_session_start(uint8_t series_mask, uint8_t version);

/** @brief Feed one validated type-0x07 frame (raw bytes incl. CRC). */
void baseline_handle_chunk(const uint8_t *frame, int len);

/** @return true while a session is in progress */
bool baseline_session_active(void);

/** @return bitmask of requested series still NOT fully received */
uint8_t baseline_session_pending_mask(void);

/** @return true when every requested series has been received */
bool baseline_session_complete(void);

/**
 * @brief Persist all completed series of the current session to NVS + RAM,
 *        then end the session.
 * @return bitmask of series actually committed (0 = nothing saved)
 */
uint8_t baseline_commit(void);

/** @brief Discard the in-progress session (no persistence). */
void baseline_session_reset(void);

/** @brief Erase all stored baselines (NVS + RAM). */
void baseline_clear_all(void);

/* ──────────── Interpolation & decision ──────────── */

/**
 * @brief Piecewise-linear interpolation of a series at `slot`.
 *        Clamps at the first/last point (no extrapolation).
 * @return false if the series has no valid baseline
 */
bool baseline_interp(uint8_t series, uint8_t slot, uint8_t *out);

/**
 * @brief Interpolate all series at `slot`.
 * @param out  Array[BASELINE_SERIES_COUNT]; entries for missing series are 0
 * @return bitmask of series successfully interpolated
 */
uint8_t baseline_eval_all(uint8_t slot, uint8_t out[BASELINE_SERIES_COUNT]);

/**
 * @brief Decide whether current readings deviate from the baseline enough to
 *        warrant an uplink.
 *
 * For every series with a valid baseline:
 *     dev = |measured - interp(slot)|
 * and the node should send when dev >= BASE_TOL_<series>.
 *
 * @param changed_mask  Optional out: bitmask of series that exceeded tolerance
 * @return true if any series exceeded its tolerance
 */
bool baseline_should_send(const sensor_data_t *data, uint8_t slot,
                          uint8_t *changed_mask);

#ifdef __cplusplus
}
#endif

#endif /* BASELINE_H */
