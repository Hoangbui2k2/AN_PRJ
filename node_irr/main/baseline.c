/**
 * @file baseline.c
 * @brief Node-side baseline storage, chunk reassembly and interpolation.
 *
 * The server sends up to 96 sample points per sensor series (temp/hum/soil)
 * over the fixed 15-minute time axis (slot 0..95) — one point per slot at most,
 * usually far fewer (sparse). The node reassembles the
 * chunks, stores them in NVS and then uses piecewise-linear interpolation to
 * decide whether the current readings deviate enough to send an uplink.
 */

#include "baseline.h"
#include "crc.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "BASELINE";

#define NVS_NS          "node"
#define NVS_KEY_SERIES  "bl_s%u"     /* bl_s0 .. bl_s2 */

/* ──────────── Committed baselines (loaded from NVS at init) ──────────── */
static baseline_series_t s_series[BASELINE_SERIES_COUNT];

/* ──────────── In-progress reassembly session ────────────
 * Must survive deep sleep (a transfer may span wake cycles), so it lives in
 * RTC memory. It is deliberately NOT persisted to NVS until complete. */
RTC_DATA_ATTR static struct {
    bool    active;
    uint8_t want_mask;                              /* requested series bitmask */
    uint8_t version;
    uint8_t total[BASELINE_SERIES_COUNT];           /* chunks per series */
    uint16_t recv[BASELINE_SERIES_COUNT];           /* received-chunk bitmask (16 bits) */
    uint8_t count[BASELINE_SERIES_COUNT];           /* points received */
    uint8_t t[BASELINE_SERIES_COUNT][BASELINE_MAX_POINTS];
    uint8_t y[BASELINE_SERIES_COUNT][BASELINE_MAX_POINTS];
} s_sess;

/* ──────────── Helpers ──────────── */

static int series_nvs_key(uint8_t series, char *buf, size_t len)
{
    return snprintf(buf, len, NVS_KEY_SERIES, (unsigned)series);
}

static bool series_chunks_complete(uint8_t series)
{
    uint8_t total = s_sess.total[series];
    if (total == 0 || total > BASELINE_MAX_CHUNKS) return false;
    return s_sess.recv[series] == (uint16_t)((1u << total) - 1u);
}

static uint8_t session_done_mask(void)
{
    uint8_t done = 0;
    for (uint8_t s = 0; s < BASELINE_SERIES_COUNT; s++) {
        if (series_chunks_complete(s)) done |= (uint8_t)(1u << s);
    }
    return done;
}

/* ──────────── NVS persistence ──────────── */

static bool load_series_from_nvs(uint8_t series, baseline_series_t *out)
{
    char key[16];
    series_nvs_key(series, key, sizeof(key));

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }

    size_t len = sizeof(baseline_series_t);
    baseline_series_t tmp;
    esp_err_t err = nvs_get_blob(h, key, &tmp, &len);
    nvs_close(h);

    if (err != ESP_OK || len != sizeof(baseline_series_t)) {
        return false;
    }
    if (tmp.version == 0 || tmp.count == 0 ||
        tmp.count > BASELINE_MAX_POINTS) {
        return false;
    }

    *out = tmp;
    return true;
}

static bool save_series_to_nvs(uint8_t series, const baseline_series_t *in)
{
    char key[16];
    series_nvs_key(series, key, sizeof(key));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_blob(h, key, in, sizeof(baseline_series_t));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS save series %u failed: %s",
                 series, esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "Saved series %u to NVS: v%u, %u points",
             series, in->version, in->count);
    return true;
}

bool baseline_init(void)
{
    bool any = false;
    for (uint8_t s = 0; s < BASELINE_SERIES_COUNT; s++) {
        if (load_series_from_nvs(s, &s_series[s])) {
            ESP_LOGI(TAG, "Loaded series %u from NVS: v%u, %u points",
                     s, s_series[s].version, s_series[s].count);
            any = true;
        } else {
            memset(&s_series[s], 0, sizeof(baseline_series_t));
        }
    }
    if (!any) {
        ESP_LOGI(TAG, "No baseline in NVS - provisioning required");
    }
    return any;
}

bool baseline_series_valid(uint8_t series)
{
    if (series >= BASELINE_SERIES_COUNT) return false;
    return s_series[series].version != 0 && s_series[series].count > 0;
}

uint8_t baseline_version(uint8_t series)
{
    if (series >= BASELINE_SERIES_COUNT) return 0;
    return s_series[series].version;
}

uint8_t baseline_valid_mask(void)
{
    uint8_t mask = 0;
    for (uint8_t s = 0; s < BASELINE_SERIES_COUNT; s++) {
        if (baseline_series_valid(s)) mask |= (uint8_t)(1u << s);
    }
    return mask;
}

/* ──────────── Reassembly ──────────── */

void baseline_session_start(uint8_t series_mask, uint8_t version)
{
    memset((void *)&s_sess, 0, sizeof(s_sess));
    s_sess.active    = true;
    s_sess.want_mask = series_mask & 0x07u;
    s_sess.version   = version;
    ESP_LOGI(TAG, "Baseline session start: mask=0x%02X v%u",
             s_sess.want_mask, version);
}

void baseline_session_reset(void)
{
    memset((void *)&s_sess, 0, sizeof(s_sess));
}

bool baseline_session_active(void)
{
    return s_sess.active;
}

uint8_t baseline_session_pending_mask(void)
{
    if (!s_sess.active) return 0;
    return (uint8_t)(s_sess.want_mask & ~session_done_mask());
}

bool baseline_session_complete(void)
{
    if (!s_sess.active) return false;
    return (s_sess.want_mask & ~session_done_mask()) == 0;
}

void baseline_handle_chunk(const uint8_t *frame, int len)
{
    if (frame == NULL) return;

    /* Layout: dest|type|series|version|seq|total|n_points|payload|crc */
    if (len < 8) return;

    uint8_t series  = frame[2];
    uint8_t version = frame[3];
    uint8_t seq     = frame[4];
    uint8_t total   = frame[5];
    uint8_t n       = frame[6];

    if (series >= BASELINE_SERIES_COUNT) return;
    if (n == 0 || n > BASELINE_POINTS_PER_CHUNK) return;
    if (total == 0 || total > BASELINE_MAX_CHUNKS || seq >= total) return;
    if (len < 7 + 2 * (int)n + 1) return;

    /* Verify CRC (last byte = XOR of all preceding bytes) */
    if (crc8_xor(frame, (uint8_t)(len - 1)) != frame[len - 1]) {
        ESP_LOGW(TAG, "Chunk CRC mismatch (series %u seq %u)", series, seq);
        return;
    }

    /* Ignore a chunk that is OLDER than the series already committed in NVS —
     * the gateway may still echo a previous transfer. A newer version (or a
     * re-send of the same version after a reset) opens a fresh session below. */
    if (version < baseline_version(series)) {
        ESP_LOGW(TAG, "Chunk v%u older than stored v%u (series %u) - ignored",
                 version, baseline_version(series), series);
        return;
    }

    /* Version change (or no session yet) → start a fresh session automatically.
     * This is the push path: the server sends a new table, the gateway dribbles
     * it down and the node stores it WITHOUT having requested anything.
     * want_mask starts EMPTY so the session completes as soon as the series the
     * server actually sent are whole (a push may carry fewer than 3 series);
     * the boot-sync flow sets 0x07 explicitly when it wants all three. */
    if (!s_sess.active || s_sess.version != version) {
        baseline_session_start(0x00u, version);
    }

    if (!(s_sess.want_mask & (uint8_t)(1u << series))) {
        /* Server sent a series we did not ask for — accept it anyway. */
        s_sess.want_mask |= (uint8_t)(1u << series);
    }

    s_sess.total[series] = total;
    if (s_sess.recv[series] & (uint16_t)(1u << seq)) {
        return;                                      /* duplicate chunk */
    }
    s_sess.recv[series] |= (uint16_t)(1u << seq);

    for (uint8_t i = 0; i < n; i++) {
        uint8_t t = frame[7 + 2 * i];
        uint8_t y = frame[7 + 2 * i + 1];
        int idx = (int)seq * BASELINE_POINTS_PER_CHUNK + i;
        if (idx >= BASELINE_MAX_POINTS) break;
        s_sess.t[series][idx] = t;
        s_sess.y[series][idx] = y;
        if ((uint8_t)(idx + 1) > s_sess.count[series]) {
            s_sess.count[series] = (uint8_t)(idx + 1);
        }
    }

    ESP_LOGI(TAG, "Chunk series=%u v%u seq=%u/%u n=%u (count=%u, recv=0x%04X)",
             series, version, seq, total, n,
             s_sess.count[series], (unsigned)s_sess.recv[series]);
}

uint8_t baseline_commit(void)
{
    if (!s_sess.active) return 0;

    uint8_t done = session_done_mask();
    uint8_t saved_mask = 0;

    for (uint8_t s = 0; s < BASELINE_SERIES_COUNT; s++) {
        if (!(done & (uint8_t)(1u << s))) continue;

        baseline_series_t tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.version = s_sess.version ? s_sess.version : 1;
        tmp.count   = s_sess.count[s];
        memcpy(tmp.t, s_sess.t[s], sizeof(tmp.t));
        memcpy(tmp.y, s_sess.y[s], sizeof(tmp.y));

        if (save_series_to_nvs(s, &tmp)) {
            s_series[s] = tmp;
            saved_mask |= (uint8_t)(1u << s);
        }
    }

    if (saved_mask) {
        ESP_LOGI(TAG, "Baseline committed (mask=0x%02X), valid mask now 0x%02X",
                 saved_mask, baseline_valid_mask());
    } else {
        /* Không ghi được gì: hoặc chưa series nào hoàn tất, hoặc NVS lỗi.
         * GIỮ session lại để còn thử lưu tiếp — nếu reset ở đây thì toàn bộ
         * dữ liệu vừa nhận bị mất và node KHÔNG BAO GIỜ gửi 0x08 'done'. */
        if (done != 0) {
            ESP_LOGE(TAG, "Baseline commit: NVS save failed (mask=0x%02X) - session kept",
                     done);
        }
        return 0;
    }

    baseline_session_reset();
    return saved_mask;
}

void baseline_clear_all(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        for (uint8_t s = 0; s < BASELINE_SERIES_COUNT; s++) {
            char key[16];
            series_nvs_key(s, key, sizeof(key));
            nvs_erase_key(h, key);
        }
        nvs_commit(h);
        nvs_close(h);
    }
    memset(s_series, 0, sizeof(s_series));
    baseline_session_reset();
    ESP_LOGW(TAG, "All baselines cleared");
}

/* ──────────── Interpolation ──────────── */

bool baseline_interp(uint8_t series, uint8_t slot, uint8_t *out)
{
    if (out == NULL || series >= BASELINE_SERIES_COUNT) return false;

    const baseline_series_t *s = &s_series[series];
    if (s->version == 0 || s->count == 0) return false;

    /* Clamp at both ends — never extrapolate. */
    if (slot <= s->t[0]) {
        *out = s->y[0];
        return true;
    }
    if (slot >= s->t[s->count - 1]) {
        *out = s->y[s->count - 1];
        return true;
    }

    for (uint8_t i = 0; i + 1 < s->count; i++) {
        int32_t t1 = s->t[i];
        int32_t t2 = s->t[i + 1];
        if (slot < t1 || slot > t2) continue;

        int32_t dt = t2 - t1;
        if (dt <= 0) {                                /* guard div-by-zero */
            *out = s->y[i];
            return true;
        }

        int32_t dx = (int32_t)slot - t1;
        int32_t dy = (int32_t)s->y[i + 1] - (int32_t)s->y[i];

        /* Round half away from zero, computed on the magnitude so the result
         * is symmetric for negative dy (e.g. falling temperature). */
        int64_t num  = (int64_t)dy * (int64_t)dx;
        uint32_t half = (uint32_t)dt / 2u;
        int32_t q;
        if (num >= 0) {
            q =  (int32_t)(((uint64_t)num + half) / (uint32_t)dt);
        } else {
            q = -(int32_t)(((uint64_t)(-num) + half) / (uint32_t)dt);
        }

        int32_t v = (int32_t)s->y[i] + q;
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        *out = (uint8_t)v;
        return true;
    }

    /* Should not happen (count>=2 and slot inside range) */
    *out = s->y[s->count - 1];
    return true;
}

uint8_t baseline_eval_all(uint8_t slot, uint8_t out[BASELINE_SERIES_COUNT])
{
    uint8_t mask = 0;
    for (uint8_t s = 0; s < BASELINE_SERIES_COUNT; s++) {
        out[s] = 0;
        if (baseline_interp(s, slot, &out[s])) {
            mask |= (uint8_t)(1u << s);
        }
    }
    return mask;
}

/* ──────────── Send decision ──────────── */

static int clamp_byte(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

bool baseline_should_send(const sensor_data_t *data, uint8_t slot,
                          uint8_t *changed_mask)
{
    if (changed_mask) *changed_mask = 0;
    if (data == NULL) return false;

    static const uint8_t tol[BASELINE_SERIES_COUNT] = {
        BASE_TOL_TEMP, BASE_TOL_HUM, BASE_TOL_SOIL
    };

    /* Measured values in the same encoding as the baseline y values. */
    int meas[BASELINE_SERIES_COUNT];
    meas[BASELINE_SERIES_TEMP] = clamp_byte((int)(data->temperature + 40.5f));
    meas[BASELINE_SERIES_HUM]  = clamp_byte((int)(data->humidity + 0.5f));
    meas[BASELINE_SERIES_SOIL] = clamp_byte((int)data->soil_moisture_pct);

    bool    dht_ok  = (data->sensor_error & 0x01) == 0;
    bool    soil_ok = (data->sensor_error & 0x02) == 0;

    uint8_t mask = 0;
    for (uint8_t s = 0; s < BASELINE_SERIES_COUNT; s++) {
        /* Skip a series whose sensor failed this cycle. */
        if ((s == BASELINE_SERIES_TEMP || s == BASELINE_SERIES_HUM) && !dht_ok) {
            continue;
        }
        if (s == BASELINE_SERIES_SOIL && !soil_ok) continue;

        uint8_t exp;
        if (!baseline_interp(s, slot, &exp)) continue;   /* no baseline → skip */

        int dev = meas[s] - (int)exp;
        if (dev < 0) dev = -dev;
        if (dev >= (int)tol[s]) {
            mask |= (uint8_t)(1u << s);
            ESP_LOGI(TAG, "Baseline dev: series=%u slot=%u meas=%d exp=%u dev=%d tol=%u",
                     s, slot, meas[s], exp, dev, tol[s]);
        }
    }

    if (changed_mask) *changed_mask = mask;
    return mask != 0;
}
