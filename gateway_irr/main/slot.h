#ifndef SLOT_H
#define SLOT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed 15-minute time axis: t = 0..95, 96 slots/day. */
#define SLOTS_PER_DAY 96
#define SLOT_MINUTES  15
#define SLOT_MS       900000UL   /* 15 * 60 * 1000 */

#define SLOT_MAX      95

/**
 * @brief Initialise the slot manager. The slot base starts at "now"; call
 *        slot_sync_reset() when the server issues a sync.
 */
void slot_init(void);

/**
 * @brief Bắt đầu SNTP để t bám GIỜ TRONG NGÀY (t = phút trong ngày / 15).
 *
 * Baseline của node có thể được cấp ở bất kỳ thời điểm nào trong ngày, nên t
 * phải là mốc thời gian thực (0..95) chứ không phải "uptime kể từ lúc gateway
 * khởi động lại" — nếu không, node so sánh nội suy sai thời điểm trong ngày.
 */
void slot_time_sync_start(void);

/** @return true khi đã có giờ thực (SNTP) */
bool slot_time_synced(void);

/**
 * @brief Server báo "hiện tại là slot N trong ngày" (sync_slot kèm "slot":N).
 *        Lưu offset so với giờ UTC để t khớp giờ địa phương.
 */
void slot_set_time_of_day(uint8_t slot);

/**
 * @brief Reset the slot base so that the current instant becomes t = 0.
 * @param slot_ms Slot width in ms (0 → default SLOT_MS)
 */
void slot_sync_reset(uint32_t slot_ms);

/**
 * @brief Current slot 0..95 derived from the monotonic clock since the last
 *        sync. Also detects the 95 -> 0 wrap (see slot_wrapped()).
 */
uint8_t slot_current(void);

/** @return true once after the slot counter wrapped 95 -> 0. */
bool slot_wrapped(void);

/** @brief Clear the wrap flag after the boundary broadcast has been issued. */
void slot_clear_wrap(void);

/** @return configured slot width in ms */
uint32_t slot_width_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* SLOT_H */
