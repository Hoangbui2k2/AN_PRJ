#include "slot.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include <time.h>

static const char *TAG = "SLOT";

static uint64_t s_sync_base_us = 0;   /* monotonic time of t = 0 */
static uint32_t s_slot_ms      = SLOT_MS;
static uint8_t  s_last_slot    = 0;
static bool     s_wrapped      = false;
static int      s_tod_offset_slots = 0;  /* giờ địa phương − giờ UTC (theo slot) */

void slot_time_sync_start(void)
{
    if (esp_sntp_enabled()) return;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started - t sẽ bám giờ trong ngày (chưa synced thì dùng uptime)");
}

bool slot_time_synced(void)
{
    if (!esp_sntp_enabled()) return false;
    return esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED;
}

static uint8_t slot_from_wall_clock(void)
{
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);

    int day_min = SLOTS_PER_DAY * SLOT_MINUTES;
    int minutes = tm.tm_hour * 60 + tm.tm_min + s_tod_offset_slots * SLOT_MINUTES;
    minutes %= day_min;
    if (minutes < 0) minutes += day_min;
    return (uint8_t)(minutes / SLOT_MINUTES);
}

void slot_set_time_of_day(uint8_t slot)
{
    if (slot > SLOT_MAX) slot = SLOT_MAX;
    if (!slot_time_synced()) {
        ESP_LOGW(TAG, "sync_slot theo giờ trong ngày cần SNTP - chưa có giờ thực");
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    int utc_slot = (tm.tm_hour * 60 + tm.tm_min) / SLOT_MINUTES;
    s_tod_offset_slots = (int)slot - utc_slot;
    ESP_LOGI(TAG, "t theo GIỜ TRONG NGÀY: UTC slot %d -> %u (offset %+d slot)",
             utc_slot, slot, s_tod_offset_slots);
}

void slot_init(void)
{
    s_sync_base_us = esp_timer_get_time();
    s_slot_ms      = SLOT_MS;
    s_last_slot    = 0;
    s_wrapped      = false;
    ESP_LOGI(TAG, "Slot manager init (slot_ms=%lu, %d slots/day)",
             (unsigned long)s_slot_ms, SLOTS_PER_DAY);
    slot_time_sync_start();
}

void slot_sync_reset(uint32_t slot_ms)
{
    if (slot_ms == 0) slot_ms = SLOT_MS;
    s_slot_ms      = slot_ms;
    s_sync_base_us = esp_timer_get_time();
    s_last_slot    = 0;
    s_wrapped      = false;
    ESP_LOGI(TAG, "Slot base reset to 0 (slot_ms=%lu)", (unsigned long)s_slot_ms);
}

uint8_t slot_current(void)
{
    uint8_t cur;

    if (slot_time_synced()) {
        /* t = slot của GIỜ TRONG NGÀY (không reset về 0 khi gateway reboot).
         * Nhờ vậy node được cấp baseline lúc 14:00 sẽ có t ≈ 56, và phần nội
         * suy so sánh đúng thời điểm trong ngày. */
        cur = slot_from_wall_clock();
        static bool logged = false;
        if (!logged) {
            logged = true;
            ESP_LOGI(TAG, "Đã có giờ thực (SNTP): t hiện tại = %u (giờ trong ngày)", cur);
        }
    } else {
        /* Fallback: chưa có giờ thực → đếm từ lúc khởi động. */
        uint64_t elapsed_ms = (esp_timer_get_time() - s_sync_base_us) / 1000ULL;
        cur = (uint8_t)((elapsed_ms / (uint64_t)s_slot_ms) % SLOTS_PER_DAY);
    }

    if (cur < s_last_slot) {
        s_wrapped = true;      /* crossed 95 -> 0 */
    }
    s_last_slot = cur;
    return cur;
}

bool slot_wrapped(void)
{
    return s_wrapped;
}

void slot_clear_wrap(void)
{
    s_wrapped = false;
}

uint32_t slot_width_ms(void)
{
    return s_slot_ms;
}
