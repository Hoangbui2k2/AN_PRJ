/**
 * @file baseline_manager.c
 * @brief Gateway-side per-node baseline sessions.
 *
 * The server pushes baseline points over MQTT (set_baseline). The gateway keeps
 * them per node and dribbles them to the node as 0x07 chunks whenever the node
 * is awake (i.e. right after one of its uplinks), then the node confirms each
 * series with a 0x08 BASELINE_DONE uplink.
 */

#include "baseline_manager.h"
#include "lora_uart.h"
#include "node_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "BASELINE_MGR";

typedef struct {
    bool     used;
    uint8_t  node_id;
    uint8_t  version;
    uint8_t  have_mask;                                   /* series stored */
    uint8_t  done_mask;                                   /* series confirmed */
    uint8_t  total[BASELINE_SERIES_COUNT];                /* chunks per series */
    uint8_t  next_seq[BASELINE_SERIES_COUNT];             /* chunk cursor */
    uint8_t  count[BASELINE_SERIES_COUNT];                /* points */
    uint8_t  t[BASELINE_SERIES_COUNT][BASELINE_MAX_POINTS];
    uint8_t  y[BASELINE_SERIES_COUNT][BASELINE_MAX_POINTS];
} node_baseline_t;

/* One session per registered node (MAX_NODES = 11 → ~7 KB:
 * 3 series × 96 points × 2 arrays + cursors). */
static node_baseline_t s_sess[MAX_NODES];

/* ──────────── Sequential delivery ("one node at a time") ────────────
 *
 * A full table is up to 96 points/series = 10 chunks/series = 30 chunks per
 * node, and the radio is shared, so the gateway delivers ONE node's table at a
 * time. The first node with data takes the turn; the others wait in FIFO order
 * and keep receiving their empty ACK until the turn is free (or the holder
 * stalls for BASELINE_TURN_TIMEOUT_MS).
 */
static uint8_t s_turn_node        = 0xFF;   /* node holding the turn */
static int64_t s_turn_progress_us = 0;      /* last progress made on the turn */
static uint8_t s_wait[MAX_NODES];           /* FIFO of nodes waiting for a turn */
static int     s_wait_len         = 0;

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

static void wait_add(uint8_t node_id)
{
    for (int i = 0; i < s_wait_len; i++) {
        if (s_wait[i] == node_id) return;            /* already queued */
    }
    if (s_wait_len >= MAX_NODES) {
        ESP_LOGW(TAG, "Baseline wait queue full - node 0x%02X not queued", node_id);
        return;
    }
    s_wait[s_wait_len++] = node_id;
    ESP_LOGI(TAG, "Node 0x%02X queued for baseline turn (queue=%d)",
             node_id, s_wait_len);
}

static void wait_remove(uint8_t node_id)
{
    for (int i = 0; i < s_wait_len; i++) {
        if (s_wait[i] == node_id) {
            memmove(&s_wait[i], &s_wait[i + 1], (size_t)(s_wait_len - i - 1));
            s_wait_len--;
            return;
        }
    }
}

static void turn_release(void)
{
    if (s_turn_node != 0xFF) {
        ESP_LOGI(TAG, "Baseline turn released by node 0x%02X", s_turn_node);
    }
    s_turn_node = 0xFF;
}

/** Give the turn to the next queued node that still has something to deliver. */
static void turn_next(void)
{
    while (s_wait_len > 0) {
        uint8_t nid = s_wait[0];
        wait_remove(nid);
        if (baseline_pending(nid)) {
            s_turn_node        = nid;
            s_turn_progress_us = now_us();
            ESP_LOGI(TAG, "Baseline turn -> node 0x%02X", nid);
            return;
        }
    }
}

/* ──────── Pending baseline REQUESTS (relay to the server, FIFO) ────────
 *
 * The gateway is only a LoRa buffer/relay for the baseline feature:
 *   node REQ 0x09  ->  gateway forwards "request"  ->  server pushes the table
 *   ->  gateway buffers it and sends chunks  ->  node 0x08 done  ->  gateway
 *   drops the table and tells the server.
 *
 * Requests are relayed ONE AT A TIME and in arrival order: while a node is
 * being served, a later request stays here and is only forwarded to the server
 * once the previous node is done, so the server never pushes two tables at
 * once and the radio carries one node's table start-to-finish.
 */
static uint8_t s_req_node[MAX_NODES];
static uint8_t s_req_mask[MAX_NODES];
static int     s_req_len = 0;

bool baseline_req_should_relay(uint8_t node_id)
{
    return (s_turn_node == 0xFF || s_turn_node == node_id);
}

void baseline_claim_turn(uint8_t node_id)
{
    if (s_turn_node == 0xFF) {
        s_turn_node        = node_id;
        s_turn_progress_us = now_us();
        ESP_LOGI(TAG, "Baseline turn -> node 0x%02X (reserved for its request)",
                 node_id);
    }
}

void baseline_req_enqueue(uint8_t node_id, uint8_t mask)
{
    /* Bảng đã có sẵn trong bộ đệm (hoặc còn series chưa gửi xong) thì không cần
     * xếp hàng request: lượt sẽ tự tới qua turn_next()/baseline_may_flush(). */
    if (baseline_pending(node_id)) return;

    for (int i = 0; i < s_req_len; i++) {
        if (s_req_node[i] == node_id) {
            s_req_mask[i] = mask;
            return;                              /* already queued */
        }
    }
    if (s_req_len >= MAX_NODES) {
        ESP_LOGW(TAG, "Baseline request queue full - node 0x%02X not queued",
                 node_id);
        return;
    }
    s_req_node[s_req_len] = node_id;
    s_req_mask[s_req_len] = mask;
    s_req_len++;
    ESP_LOGI(TAG, "Node 0x%02X baseline request queued (queue=%d)",
             node_id, s_req_len);
}

void baseline_req_dequeue(uint8_t node_id)
{
    for (int i = 0; i < s_req_len; i++) {
        if (s_req_node[i] == node_id) {
            memmove(&s_req_node[i], &s_req_node[i + 1],
                    (size_t)(s_req_len - i - 1));
            memmove(&s_req_mask[i], &s_req_mask[i + 1],
                    (size_t)(s_req_len - i - 1));
            s_req_len--;
            return;
        }
    }
}

bool baseline_req_publish_allowed(uint8_t node_id)
{
    static uint8_t  s_pub_node[MAX_NODES];
    static uint32_t s_pub_ms[MAX_NODES];

    int idx = -1, free_idx = -1;
    for (int i = 0; i < MAX_NODES; i++) {
        if (s_pub_node[i] == node_id) { idx = i; break; }
        if (free_idx < 0 && s_pub_node[i] == 0) free_idx = i;
    }
    if (idx < 0) {
        idx = (free_idx >= 0) ? free_idx : ((int)node_id % MAX_NODES);
        s_pub_node[idx] = node_id;
        s_pub_ms[idx]   = 0;
    }

    uint32_t now_ms = (uint32_t)(now_us() / 1000);
    if (s_pub_ms[idx] != 0 &&
        (now_ms - s_pub_ms[idx]) < BASELINE_REQ_PUBLISH_MIN_MS) {
        return false;
    }
    s_pub_ms[idx] = now_ms;
    return true;
}

bool baseline_take_pending_req(uint8_t *node_id, uint8_t *mask)
{
    if (s_req_len == 0) return false;
    if (s_turn_node != 0xFF) return false;      /* còn node khác đang được cấp */

    uint8_t nid = s_req_node[0];
    uint8_t m   = s_req_mask[0];
    baseline_req_dequeue(nid);
    baseline_claim_turn(nid);

    *node_id = nid;
    *mask    = m;
    ESP_LOGI(TAG, "Relaying queued baseline request of node 0x%02X (mask=0x%02X) "
             "to server", nid, m);
    return true;
}

static node_baseline_t *find_session(uint8_t node_id)
{
    for (int i = 0; i < MAX_NODES; i++) {
        if (s_sess[i].used && s_sess[i].node_id == node_id) return &s_sess[i];
    }
    return NULL;
}

static node_baseline_t *find_or_alloc(uint8_t node_id)
{
    node_baseline_t *s = find_session(node_id);
    if (s) return s;

    for (int i = 0; i < MAX_NODES; i++) {
        if (!s_sess[i].used) {
            memset(&s_sess[i], 0, sizeof(s_sess[i]));
            s_sess[i].used = true;
            s_sess[i].node_id = node_id;
            return &s_sess[i];
        }
    }
    return NULL;
}

void baseline_manager_init(void)
{
    memset(s_sess, 0, sizeof(s_sess));
    s_turn_node        = 0xFF;
    s_turn_progress_us = 0;
    s_wait_len         = 0;
    s_req_len          = 0;
    ESP_LOGI(TAG, "Baseline manager initialized (%d node slots, 1 transfer at a time)",
             MAX_NODES);
}

bool baseline_store(uint8_t node_id, uint8_t series, uint8_t version,
                    const uint8_t *t, const uint8_t *y, uint8_t cnt)
{
    if (series >= BASELINE_SERIES_COUNT || version == 0) return false;
    if (t == NULL || y == NULL || cnt == 0 || cnt > BASELINE_MAX_POINTS) {
        return false;
    }

    node_baseline_t *s = find_or_alloc(node_id);
    if (s == NULL) {
        ESP_LOGE(TAG, "No baseline slot available for node 0x%02X", node_id);
        return false;
    }

    /* A new version replaces the whole node session. */
    if (s->version != 0 && s->version != version) {
        uint8_t keep_id = s->node_id;
        uint8_t old_ver = s->version;
        memset(s, 0, sizeof(*s));
        s->used = true;
        s->node_id = keep_id;
        ESP_LOGI(TAG, "Node 0x%02X baseline version %u -> %u (session reset)",
                 node_id, old_ver, version);
    }

    s->version = version;
    s->count[series] = cnt;
    memcpy(s->t[series], t, cnt);
    memcpy(s->y[series], y, cnt);
    s->total[series] = (uint8_t)((cnt + BASELINE_POINTS_PER_CHUNK - 1) /
                                 BASELINE_POINTS_PER_CHUNK);
    s->next_seq[series] = 0;
    s->have_mask |= (uint8_t)(1u << series);
    s->done_mask &= (uint8_t)~(1u << series);

    ESP_LOGI(TAG, "Stored node 0x%02X series=%u v%u points=%u chunks=%u",
             node_id, series, version, cnt, s->total[series]);

    /* The table the node asked for has arrived - the queued request is served. */
    baseline_req_dequeue(node_id);

    /* Sequential delivery: the first node with data takes the turn, every
     * other node waits in FIFO order until the current transfer completes. */
    if (s_turn_node == 0xFF) {
        s_turn_node        = node_id;
        s_turn_progress_us = now_us();
        ESP_LOGI(TAG, "Baseline turn -> node 0x%02X", node_id);
    } else if (s_turn_node != node_id) {
        wait_add(node_id);
    }
    return true;
}

bool baseline_pending(uint8_t node_id)
{
    node_baseline_t *s = find_session(node_id);
    if (s == NULL) return false;
    return (s->have_mask & ~s->done_mask) != 0;
}

uint8_t baseline_active_node(void)
{
    return s_turn_node;
}

bool baseline_may_flush(uint8_t node_id)
{
    if (!baseline_pending(node_id)) return false;

    /* Another node holds the turn: queue up and wait, unless that node has
     * stalled (no chunk went out for BASELINE_TURN_TIMEOUT_MS). */
    if (s_turn_node != 0xFF && s_turn_node != node_id) {
        if ((now_us() - s_turn_progress_us) <
            (int64_t)BASELINE_TURN_TIMEOUT_MS * 1000) {
            wait_add(node_id);
            return false;
        }
        ESP_LOGW(TAG, "Baseline turn for node 0x%02X stalled > %d ms - moving on",
                 s_turn_node, BASELINE_TURN_TIMEOUT_MS);
        wait_remove(s_turn_node);
        turn_release();
    }

    if (s_turn_node == 0xFF) {
        s_turn_node        = node_id;
        s_turn_progress_us = now_us();
        ESP_LOGI(TAG, "Baseline turn -> node 0x%02X", node_id);
    }
    wait_remove(node_id);
    return true;
}

int baseline_flush_for_node(uint8_t node_id, int max_chunks)
{
    node_baseline_t *s = find_session(node_id);
    if (s == NULL || max_chunks <= 0) return 0;

    int sent = 0;

    for (uint8_t ser = 0; ser < BASELINE_SERIES_COUNT && sent < max_chunks; ser++) {
        uint8_t bit = (uint8_t)(1u << ser);
        if (!(s->have_mask & bit) || (s->done_mask & bit)) continue;

        while (s->next_seq[ser] < s->total[ser] && sent < max_chunks) {
            uint8_t seq   = s->next_seq[ser];
            int     first = (int)seq * BASELINE_POINTS_PER_CHUNK;
            int     n     = (int)s->count[ser] - first;
            if (n <= 0) {
                s->next_seq[ser] = s->total[ser];
                break;
            }
            if (n > BASELINE_POINTS_PER_CHUNK) n = BASELINE_POINTS_PER_CHUNK;

            uint8_t packed[BASELINE_POINTS_PER_CHUNK * 2];
            for (int i = 0; i < n; i++) {
                packed[2 * i]     = s->t[ser][first + i];
                packed[2 * i + 1] = s->y[ser][first + i];
            }

            if (!lora_send_baseline_chunk(node_id, ser, s->version, seq,
                                          s->total[ser], packed, (uint8_t)n)) {
                ESP_LOGW(TAG, "Baseline TX failed (node 0x%02X series %u seq %u)",
                         node_id, ser, seq);
                return sent;                 /* retry on next opportunity */
            }

            s->next_seq[ser]++;
            sent++;
        }
    }

    if (sent > 0) {
        if (s_turn_node == node_id) s_turn_progress_us = now_us();
        ESP_LOGI(TAG, "Flushed %d baseline chunk(s) to node 0x%02X", sent, node_id);
    }
    return sent;
}

void baseline_rearm(uint8_t node_id)
{
    node_baseline_t *s = find_session(node_id);
    if (s == NULL) return;

    /* Đang gửi dở mà rewind ngay mỗi REQ ⇒ gateway gửi lại từ serie 0 mãi mãi
     * (node chưa bao giờ nhận đủ cả bảng) → chunk storm + task WDT reset.
     * Chỉ rewind khi lượt của node đã "nguội". */
    if (s_turn_node == node_id &&
        (now_us() - s_turn_progress_us) <
            (int64_t)BASELINE_REARM_MIN_GAP_MS * 1000) {
        ESP_LOGI(TAG, "Re-arm skipped for node 0x%02X (%lld ms since last chunk)",
                 node_id, (long long)((now_us() - s_turn_progress_us) / 1000));
        return;
    }

    for (uint8_t ser = 0; ser < BASELINE_SERIES_COUNT; ser++) {
        if ((s->have_mask & (uint8_t)(1u << ser)) &&
            !(s->done_mask & (uint8_t)(1u << ser))) {
            s->next_seq[ser] = 0;
        }
    }
    ESP_LOGI(TAG, "Baseline re-armed for node 0x%02X", node_id);
}

bool baseline_mark_done(uint8_t node_id, uint8_t series, uint8_t version)
{
    node_baseline_t *s = find_session(node_id);
    if (s == NULL || series >= BASELINE_SERIES_COUNT) {
        ESP_LOGW(TAG, "DONE node 0x%02X series %u v%u - no active session "
                 "(already complete, or the table was never stored)",
                 node_id, series, version);
        return false;
    }

    if (s->version != version) {
        ESP_LOGW(TAG, "DONE node 0x%02X series %u v%u ignored (session v%u)",
                 node_id, series, version, s->version);
        return false;
    }

    s->done_mask |= (uint8_t)(1u << series);
    ESP_LOGI(TAG, "Node 0x%02X stored series %u (done=0x%02X have=0x%02X)",
             node_id, series, s->done_mask, s->have_mask);

    if ((s->have_mask & ~s->done_mask) == 0) {
        ESP_LOGI(TAG, "Node 0x%02X baseline session COMPLETE - table dropped",
                 node_id);
        memset(s, 0, sizeof(*s));            /* free the slot */
        baseline_req_dequeue(node_id);       /* request fully served */
        /* Hand the turn to the next waiting node (sequential delivery). */
        if (s_turn_node == node_id) {
            turn_release();
            turn_next();
        } else {
            wait_remove(node_id);
        }
        return true;
    }

    if (s_turn_node == node_id) s_turn_progress_us = now_us();   /* progress made */
    return false;
}

uint8_t baseline_done_mask(uint8_t node_id)
{
    node_baseline_t *s = find_session(node_id);
    return s ? s->done_mask : 0;
}

uint8_t baseline_have_mask(uint8_t node_id)
{
    node_baseline_t *s = find_session(node_id);
    return s ? s->have_mask : 0;
}

int baseline_flush_pending_count(uint8_t node_id)
{
    node_baseline_t *s = find_session(node_id);
    if (s == NULL) return 0;

    int n = 0;
    for (uint8_t ser = 0; ser < BASELINE_SERIES_COUNT; ser++) {
        uint8_t bit = (uint8_t)(1u << ser);
        if (!(s->have_mask & bit) || (s->done_mask & bit)) continue;
        if (s->next_seq[ser] < s->total[ser]) {
            n += (int)(s->total[ser] - s->next_seq[ser]);
        }
    }
    return n;
}

void baseline_turn_watchdog(void)
{
    if (s_turn_node == 0xFF) return;

    int64_t idle_ms = (now_us() - s_turn_progress_us) / 1000;

    /* Đã gửi hết chunk còn thiếu nhưng node chưa xác nhận (DONE có thể mất
     * frame trên không) ⇒ chỉ chờ ngắn, không để node này chặn những node khác
     * cả 120 s. Session KHÔNG bị xoá: nếu DONE tới muộn vẫn được tính. */
    bool waiting_done = baseline_pending(s_turn_node) &&
                        baseline_flush_pending_count(s_turn_node) == 0;
    int64_t limit_ms  = waiting_done ? (int64_t)BASELINE_DONE_WAIT_MS
                                     : (int64_t)BASELINE_TURN_TIMEOUT_MS;

    if (idle_ms < limit_ms) return;

    if (waiting_done) {
        ESP_LOGW(TAG, "Node 0x%02X chưa xác nhận DONE sau %d ms (đã gửi hết chunk)"
                 " - trả lượt cho node kế tiếp", s_turn_node, BASELINE_DONE_WAIT_MS);
    } else {
        ESP_LOGW(TAG, "Baseline turn of node 0x%02X stalled > %d ms - moving on",
                 s_turn_node, BASELINE_TURN_TIMEOUT_MS);
    }

    wait_remove(s_turn_node);
    turn_release();
    turn_next();
}

void baseline_clear_node(uint8_t node_id)
{
    node_baseline_t *s = find_session(node_id);
    if (s) memset(s, 0, sizeof(*s));

    wait_remove(node_id);
    baseline_req_dequeue(node_id);
    if (s_turn_node == node_id) {
        turn_release();
        turn_next();
    }
}
