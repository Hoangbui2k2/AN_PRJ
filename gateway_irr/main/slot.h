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

/* Múi giờ địa phương cho slot_from_wall_clock(). POSIX TZ: "std offset" với
 * offset DƯƠNG = phía TÂY của UTC, nên Việt Nam (UTC+7, không DST) viết là -7. */
#define SLOT_TIMEZONE  "ICT-7"

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
 * @brief Thăm dò SNTP đúng một chỗ (gọi định kỳ từ watchdog gateway).
 *
 * esp_sntp_get_sync_status() là lệnh CONSUMING: trả COMPLETED đúng một lần rồi
 * reset cờ. Vì vậy chỉ hàm này được gọi nó; khi thấy COMPLETED sẽ chốt
 * s_have_real_time và slot_current()/slot_time_synced() chuyển sang giờ thực.
 */
void slot_sntp_poll(void);

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

/**
 * @brief Đọc slot + phase từ CÙNG một mẫu thời gian (nhất quán với nhau).
 *
 * Dùng khi gửi downlink: slot và phase phải khớp cùng một thời điểm, nếu không
 * ngay tại biên 15 phút có thể gửi slot=N nhưng phase đã gần 900000 ms.
 * Khi đã có giờ thực thì tính từ đồng hồ (giây+phần giây); chưa thì từ uptime.
 */
void slot_snapshot(uint8_t *slot_out, uint32_t *phase_out);

#ifdef __cplusplus
}
#endif

#endif /* SLOT_H */
