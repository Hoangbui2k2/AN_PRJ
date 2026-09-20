#ifndef BASELINE_MANAGER_H
#define BASELINE_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Number of baseline chunks pushed per uplink opportunity. Keeps each downlink
 * burst short enough to fit the node's listen window. */
#define BASELINE_CHUNKS_PER_FLUSH 3

/* A node re-requesting after a partial transfer keeps the turn, but a node that
 * simply stops uplinking must not block the others forever: the turn is
 * released after this much idle time so the next node in the queue starts. */
#define BASELINE_TURN_TIMEOUT_MS  120000

/* Không rewind con trỏ chunk nếu lượt của node vừa có tiến độ trong khoảng này.
 * Rewind ngay mỗi REQ ⇒ gateway gửi lại từ serie 0 mãi mãi trong khi node chưa
 * kịp nhận đủ 3 serie (chunk storm → task WDT reset gateway). */
#define BASELINE_REARM_MIN_GAP_MS  2500

/* Đã gửi HẾT chunk cho node nhưng chưa nhận đủ 0x08 (mất 1 frame DONE trên
 * không): không để node đó giữ lượt tới 120 s. Hết thời gian này → trả lượt cho
 * node kế tiếp (session vẫn giữ nguyên, DONE tới muộn vẫn được ghi nhận). */
#define BASELINE_DONE_WAIT_MS  15000

/* Giãn cách tối thiểu giữa 2 lần chuyển tiếp 'request' của CÙNG một node lên
 * server (node REQ lại vài giây một lần khi chưa có bảng). */
#define BASELINE_REQ_PUBLISH_MIN_MS  20000

/**
 * @brief Initialise the per-node baseline session table.
 */
void baseline_manager_init(void);

/**
 * @brief Store/replace one baseline series for a node (from MQTT set_baseline).
 *
 * Points must be validated by the caller (ascending t in 0..95,
 * cnt <= BASELINE_MAX_POINTS = 96).
 *
 * @param node_id Target node
 * @param series  BASELINE_SERIES_TEMP/HUM/SOIL
 * @param version Baseline version (must be > 0)
 * @param t       Array of slot indices (ascending)
 * @param y       Array of 1-byte values
 * @param cnt     Number of points
 * @return true if accepted
 */
bool baseline_store(uint8_t node_id, uint8_t series, uint8_t version,
                    const uint8_t *t, const uint8_t *y, uint8_t cnt);

/** @return true if the node has at least one series still to deliver/confirm */
bool baseline_pending(uint8_t node_id);

/**
 * @brief Send the next baseline chunks for a node.
 *
 * Called when the node is known to be awake (after an uplink). Advances an
 * internal per-series chunk cursor, so successive calls send successive chunks.
 *
 * @param node_id   Target node
 * @param max_chunks Max chunks to send in this burst
 * @return number of chunks actually sent
 */
int baseline_flush_for_node(uint8_t node_id, int max_chunks);

/**
 * @brief Node currently allowed to receive baseline chunks (0xFF = none).
 *
 * Delivery is SEQUENTIAL: only one node receives chunks at a time so the
 * shared radio carries one node's table start-to-finish.
 */
uint8_t baseline_active_node(void);

/**
 * @brief May this node receive baseline chunks right now?
 *
 * Returns true when the node already holds the turn or the turn is free (in
 * which case it takes it). When another node holds the turn the caller should
 * NOT send chunks — the node is queued and gets its turn when the current
 * transfer completes or stalls (BASELINE_TURN_TIMEOUT_MS).
 */
bool baseline_may_flush(uint8_t node_id);

/**
 * @brief May this node's baseline REQUEST be relayed to the server right now?
 *
 * The gateway is only a LoRa BUFFER: it never originates a baseline. A node's
 * request is forwarded to the server, the server pushes the table, the gateway
 * buffers it for that node, sends it, and drops it when the node reports done.
 * Requests are relayed ONE AT A TIME: this returns true only when no other
 * node is currently being served (turn free or already this node's turn).
 */
bool baseline_req_should_relay(uint8_t node_id);

/**
 * @brief Take the turn for a node (no-op when another node already holds it).
 *
 * Called when a request is relayed to the server so that later requesters stay
 * in the queue until this node's transfer is complete.
 */
void baseline_claim_turn(uint8_t node_id);

/**
 * @brief Queue a node's request until the current transfer is done.
 *        Idempotent: re-queueing the same node only refreshes its mask.
 */
void baseline_req_enqueue(uint8_t node_id, uint8_t mask);

/** @brief Forget a queued request (already relayed, or the table arrived). */
void baseline_req_dequeue(uint8_t node_id);

/**
 * @brief Pop the next queued request that may be relayed now (turn free) and
 *        take the turn for it.
 *
 * @param[out] node_id Node whose request should be sent to the server
 * @param[out] mask    Series mask the node asked for
 * @return true when the outputs were filled
 */
bool baseline_take_pending_req(uint8_t *node_id, uint8_t *mask);

/**
 * @brief Rewind the chunk cursor so the whole series is sent again.
 *        Used when the node re-requests after a partial/failed transfer.
 */
void baseline_rearm(uint8_t node_id);

/**
 * @brief Mark a series as stored by the node (uplink 0x08 BASELINE_DONE).
 * @return true if this completed the node's whole baseline session
 */
bool baseline_mark_done(uint8_t node_id, uint8_t series, uint8_t version);

/**
 * @brief Có được publish 'request' của node này lên server ngay bây giờ không?
 *
 * Node REQ lại mỗi vài giây khi chưa có bảng; gateway chỉ chuyển tiếp tối đa
 * một lần mỗi BASELINE_REQ_PUBLISH_MIN_MS cho mỗi node để không dội MQTT.
 * Trả về true = nên publish (đồng thời ghi mốc thời gian).
 */
bool baseline_req_publish_allowed(uint8_t node_id);

/** @return bitmask of series already confirmed by the node */
uint8_t baseline_done_mask(uint8_t node_id);

/** @return bitmask of series the gateway is holding for the node (0 = no session) */
uint8_t baseline_have_mask(uint8_t node_id);

/** @brief Drop all session state for a node. */
void baseline_clear_node(uint8_t node_id);

/**
 * @brief Số chunk còn CHƯA gửi cho node (0 = đã gửi hết, đang chờ DONE).
 */
int baseline_flush_pending_count(uint8_t node_id);

/**
 * @brief Kiểm tra lượt có bị "treo" không và trả lại khi cần.
 *
 * Gọi thường xuyên (đầu mỗi ack_or_flush_node). Hai ngưỡng:
 *  - đang còn chunk chưa gửi hoặc chưa có bảng (chờ server push) → BASELINE_TURN_TIMEOUT_MS;
 *  - đã gửi hết chunk nhưng thiếu frame DONE → BASELINE_DONE_WAIT_MS (ngắn hơn).
 */
void baseline_turn_watchdog(void);

#ifdef __cplusplus
}
#endif

#endif /* BASELINE_MANAGER_H */
