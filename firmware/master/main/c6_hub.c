#include "c6_hub.h"
#include "c6_link.h"
#include "c6_store.h"
#include "audio_player.h"
#include "espnow_link.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "fleet_proto.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "link_proto.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "status_led.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "c6_hub";

#define NVS_NS        "c6fleet"
#define BEACON_MS     2000u
#define NODE_LIVE_MS  8000u     /* no traffic this long -> offline */
#define PAIRING_OPEN_MS (5u * 60u * 1000u)   /* an opened pairing closes by itself after this */

static SemaphoreHandle_t s_lock;
static race_timing_t     s_rt;
static c6_node_t         s_nodes[C6_MAX_NODES];
static uint8_t           s_num_nodes;
static bool              s_pairing;           /* closed at every boot: an admin opens it, for PAIRING_OPEN_MS */
static uint32_t          s_pairing_until;     /* now_ms() when an open pairing closes again */
static uint32_t          s_beacon_seq;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ---- debug mode of ONE remote gate (the admin web's Debug panel) ----
 * Pulled, not pushed: a node streaming its C5's console would load the air for every gate. The
 * target is one node at a time; while a page polls /api/log (the lease), dbg_task asks it for the
 * next bytes, one LOG_REQ in flight (stop-and-wait, 20 per second at most), and keeps the text in a
 * ring for the page. Everything below is under s_lock; no frame goes out under it. */
#define DBG_RING        4096u
#define DBG_LEASE_MS    10000u    /* no /api/log from a page this long: the master switches it off */
#define DBG_TICK_MS     50u
#define DBG_IDLE_MS     100u      /* between two polls when the last chunk was not full */
#define DBG_REPLY_MS    300u      /* a LOG_REQ unanswered this long is asked again */
#define DBG_ON_GRACE_MS 1500u     /* a debug-off chunk right after our DEBUG on: it is not there yet */
static char          s_dlog[DBG_RING];
static lp_logring_t  s_dring = { .buf = s_dlog, .cap = DBG_RING };
static int           s_dgate = -1;        /* the target gate, -1 none */
static uint8_t       s_dmac[6];
static uint32_t      s_dexpected;         /* the node's position of the next byte we have not got */
static bool          s_dinflight;
static uint32_t      s_dreq_ms, s_don_ms, s_dpage_ms;
static bool          s_dfast;             /* the last chunk was full: ask again at once */

static void dbg_note_lost(unsigned long bytes) {   /* s_lock held: a line in the log itself */
    lp_text_t t;
    const int n = snprintf(t.text, sizeof t.text, "\n[log: %lu bytes lost]\n", bytes);
    if (n <= 0) return;
    t.len = (uint8_t)(n < (int)sizeof t.text ? n : (int)sizeof t.text - 1);
    lp_logring_put(&s_dring, &t);
}

static void send_debug_frame(const uint8_t mac[6], bool on) {
    uint8_t buf[FP_MAX_FRAME];
    const size_t n = fp_encode_debug(buf, sizeof buf, on);
    if (n) espnow_link_send(mac, buf, n);
}

static void audio_start(void) {
    const audio_player_config_t cfg = {
        .bclk_gpio = CONFIG_C6_AUDIO_BCLK_GPIO, .ws_gpio = CONFIG_C6_AUDIO_WS_GPIO,
        .dout_gpio = CONFIG_C6_AUDIO_DOUT_GPIO, .sd_gpio = CONFIG_C6_AUDIO_SD_GPIO,
        .task_priority = 5,
    };
    const esp_err_t err = audio_player_start(&cfg);
    if (err != ESP_OK) ESP_LOGW(TAG, "audio off: %s (no coin.pcm / build)", esp_err_to_name(err));
}

/* Centralized audio volume, persisted in NVS so the web setting survives a reboot. */
static void volume_apply_saved(void) {
    uint8_t v = AUDIO_VOLUME_DEFAULT;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "volume", &v); nvs_close(h); }
    audio_player_set_volume(v);
}

void c6_hub_set_volume(uint8_t pct) {
    if (pct > 100u) pct = 100u;
    audio_player_set_volume(pct);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "volume", pct); nvs_commit(h); nvs_close(h);
    }
}

/* What every remote gate runs; its re-arm drop is the fleet's (c6_store, 16 dB until an admin
 * sets one: a 0 there would turn the re-arm off on every gate). */
static lp_config_t default_config(void) {
    const lp_config_t c = {
        .alpha_milli = 250, .active_db_x10 = 80, .hysteresis_db_x10 = 30,   /* sd_config_default() */
        .drop_db_x10 = 30, .min_peak_db_x10 = 100, .refractory_ms = 1500,
        .stale_ms = 1000, .rearm_db_x10 = c6_store_rearm_x10(),
        .volume_pct = 60, .muted = 0,
    };
    return c;
}

/* Is pairing open? It closes by itself PAIRING_OPEN_MS after it was opened. s_lock held. */
static bool pairing_open(void) {
    if (s_pairing && (int32_t)(now_ms() - s_pairing_until) >= 0) s_pairing = false;
    return s_pairing;
}

/* ---- per-gate thresholds from a passage calibration (an override of the fleet's detector config) ----
 * Only the four thresholds a calibration sets; D and alpha stay the fleet's. Keyed by MAC (node ids can
 * repeat after a removal), in their own NVS key so the node table's record never changes. Merged into
 * default_config() at every push (send_config), so a re-pair, a back-online CONFIG or a change of D
 * never erases it. */
typedef struct __attribute__((packed)) { uint8_t mac[6]; int16_t active, hysteresis, drop, min_peak; } ovr_rec_t;
static ovr_rec_t s_ovr[C6_MAX_NODES];
static uint8_t   s_num_ovr;

static ovr_rec_t *find_ovr(const uint8_t mac[6]) {            /* s_lock held */
    for (unsigned i = 0; i < s_num_ovr; i++)
        if (memcmp(s_ovr[i].mac, mac, 6) == 0) return &s_ovr[i];
    return NULL;
}

static void ovr_save(void) {                                   /* s_lock held */
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (s_num_ovr) nvs_set_blob(h, "ovr", s_ovr, s_num_ovr * sizeof(ovr_rec_t));
    else nvs_erase_key(h, "ovr");
    nvs_commit(h);
    nvs_close(h);
}

static void ovr_load(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof s_ovr;
    if (nvs_get_blob(h, "ovr", s_ovr, &len) == ESP_OK && len % sizeof(ovr_rec_t) == 0) s_num_ovr = (uint8_t)(len / sizeof(ovr_rec_t));
    nvs_close(h);
}

static void ovr_set(const uint8_t mac[6], const lp_calib_t *k) {   /* s_lock held */
    ovr_rec_t *o = find_ovr(mac);
    if (!o && s_num_ovr < C6_MAX_NODES) { o = &s_ovr[s_num_ovr++]; memcpy(o->mac, mac, 6); }
    if (!o) return;
    o->active = k->active_db_x10; o->hysteresis = k->hysteresis_db_x10; o->drop = k->drop_db_x10; o->min_peak = k->min_peak_db_x10;
    ovr_save();
}

static bool ovr_drop(const uint8_t mac[6]) {                    /* s_lock held */
    ovr_rec_t *o = find_ovr(mac);
    if (!o) return false;
    *o = s_ovr[--s_num_ovr];
    ovr_save();
    return true;
}

/* ---- NVS node table ---- */
typedef struct __attribute__((packed)) {
    uint8_t mac[6], node_id, gate_index, channel;
} node_rec_t;

static void nodes_save(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    node_rec_t rec[C6_MAX_NODES];
    for (unsigned i = 0; i < s_num_nodes; i++) {
        memcpy(rec[i].mac, s_nodes[i].mac, 6);
        rec[i].node_id = s_nodes[i].node_id;
        rec[i].gate_index = s_nodes[i].gate_index;
        rec[i].channel = s_nodes[i].channel;
    }
    nvs_set_u8(h, "count", s_num_nodes);
    if (s_num_nodes) nvs_set_blob(h, "nodes", rec, s_num_nodes * sizeof(node_rec_t));
    nvs_commit(h);
    nvs_close(h);
}

static void nodes_load(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t count = 0;
    if (nvs_get_u8(h, "count", &count) == ESP_OK && count && count <= C6_MAX_NODES) {
        node_rec_t rec[C6_MAX_NODES];
        size_t len = count * sizeof(node_rec_t);
        if (nvs_get_blob(h, "nodes", rec, &len) == ESP_OK && len == count * sizeof(node_rec_t)) {
            s_num_nodes = count;
            for (unsigned i = 0; i < count; i++) {
                /* Only the assignment persists: link counters and C5 health start zeroed. */
                s_nodes[i] = (c6_node_t){ .node_id = rec[i].node_id, .gate_index = rec[i].gate_index,
                                          .channel = rec[i].channel, .cn_x10 = LP_CN_UNKNOWN,
                                          .rearm_x10 = FP_REARM_UNKNOWN };
                memcpy(s_nodes[i].mac, rec[i].mac, 6);
            }
        }
    }
    nvs_close(h);
}

static uint8_t free_node_id(void);

/* A table saved before ids were handed out free could hold two nodes with one id (a removal, then a pairing):
 * the web names nodes by id, so every later duplicate gets a free one. */
static void nodes_unique_ids(void) {
    bool changed = false;
    for (unsigned i = 1; i < s_num_nodes; i++)
        for (unsigned j = 0; j < i; j++)
            if (s_nodes[j].node_id == s_nodes[i].node_id) { s_nodes[i].node_id = free_node_id(); changed = true; break; }
    if (changed) nodes_save();
}

/* The lowest node id no paired node holds (ids name a node in the web's reorder and remove). s_lock held. */
static uint8_t free_node_id(void) {
    for (unsigned id = 0; id < 256u; id++) {
        bool used = false;
        for (unsigned i = 0; i < s_num_nodes && !used; i++) used = s_nodes[i].node_id == id;
        if (!used) return (uint8_t)id;
    }
    return 0;
}

static c6_node_t *find_by_mac(const uint8_t mac[6]) {
    for (unsigned i = 0; i < s_num_nodes; i++)
        if (memcmp(s_nodes[i].mac, mac, 6) == 0) return &s_nodes[i];
    return NULL;
}

/* Gate count = highest assigned gate + 1 (gate 0 is always the local C5), so
 * admin-assigned orders with gaps still size race_timing correctly. */
static uint8_t num_gates(void) {
    uint8_t g = 1u;
    for (unsigned i = 0; i < s_num_nodes; i++)
        if ((uint8_t)(s_nodes[i].gate_index + 1u) > g) g = (uint8_t)(s_nodes[i].gate_index + 1u);
    if (g > RT_MAX_GATES) g = RT_MAX_GATES;
    return g;
}

static void send_config(const uint8_t mac[6], uint8_t gate_index, uint8_t channel) {
    fp_config_t cfg = { .gate_index = gate_index, .channel = channel, .enable = 1, .det = default_config() };
    xSemaphoreTake(s_lock, portMAX_DELAY);          /* never held by a caller of this */
    const ovr_rec_t *o = find_ovr(mac);
    if (o) {                                        /* this gate's thresholds from a passage calibration */
        cfg.det.active_db_x10 = o->active; cfg.det.hysteresis_db_x10 = o->hysteresis;
        cfg.det.drop_db_x10 = o->drop; cfg.det.min_peak_db_x10 = o->min_peak;
    }
    xSemaphoreGive(s_lock);
    uint8_t buf[FP_MAX_FRAME];
    size_t m = fp_encode_config(buf, sizeof buf, &cfg);
    if (m) espnow_link_send(mac, buf, m);
}

static void send_config_to(const c6_node_t *n) { send_config(n->mac, n->gate_index, n->channel); }

static void send_pair_accept(const c6_node_t *n, uint32_t nonce) {
    const fp_pair_accept_t pa = { .nonce = nonce, .node_id = n->node_id,
                                  .gate_index = n->gate_index, .channel = n->channel };
    uint8_t buf[FP_MAX_FRAME];
    size_t m = fp_encode_pair_accept(buf, sizeof buf, &pa);
    if (m) espnow_link_send(n->mac, buf, m);
}

static void handle_pair(const uint8_t mac[6], const fp_pair_request_t *req) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    c6_node_t *n = find_by_mac(mac);
    if (!n) {
        if (!pairing_open() || s_num_nodes >= C6_MAX_NODES) { xSemaphoreGive(s_lock); return; }
        n = &s_nodes[s_num_nodes];
        /* Link counters and C5 health start zeroed; next free gate (gate 0 is the local C5). */
        *n = (c6_node_t){ .node_id = free_node_id(), .gate_index = num_gates(),
                          .channel = req->seen_channel, .cn_x10 = LP_CN_UNKNOWN,
                          .rearm_x10 = FP_REARM_UNKNOWN, .online = true, .last_seen_ms = now_ms() };
        memcpy(n->mac, mac, 6);
        s_num_nodes++;
        espnow_link_add_peer(mac);
        nodes_save();
        rt_init(&s_rt, &(rt_config_t){ .num_gates = num_gates() });
        ESP_LOGI(TAG, "paired node id=%u gate=%u ch=%u", n->node_id, n->gate_index, n->channel);
    }
    const c6_node_t snap = *n;
    xSemaphoreGive(s_lock);
    send_pair_accept(&snap, req->nonce);
    send_config_to(&snap);
}

static void record_pass(uint8_t gate, uint8_t channel, int32_t peak_db_x10, uint32_t fleet_ms) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    rt_cross(&s_rt, gate, channel, fleet_ms, NULL);
    xSemaphoreGive(s_lock);
    const c6_pass_t p = { .channel = channel, .peak_db_x10 = peak_db_x10,
                          .at_ms = fleet_ms, .gate = gate };
    c6_store_add_pass(&p);
    audio_player_play(AUDIO_SOUND_COIN, audio_pitch_for_channel(channel));
    status_led_pass();   /* white flash on any gate's pass */
}

/* Mark a known node live and fold in this frame's RSSI + receive count. Called
 * under s_lock for every frame from a paired node, before any per-type work.
 * True when it was offline (or not heard since our boot): it may have missed a
 * reassignment or rebooted onto stale settings, so the caller re-sends its
 * CONFIG once the lock is released. */
static bool node_touch(c6_node_t *n, int8_t rssi) {
    const bool back = !n->online;
    n->last_seen_ms = now_ms();
    n->online = true;
    n->rssi = rssi;
    n->rx_frames++;
    return back;
}

/* NODE_STATUS carries a per-node monotonic seq; a gap is an inferred loss, and a
 * seq going backwards means the node rebooted, so (re)sync the window. */
static void status_track_loss(c6_node_t *n, uint32_t seq) {
    if (!n->seq_init || seq < n->status_seq) {
        n->seq_init = true; n->status_rx = 1; n->status_lost = 0;
    } else {
        n->status_rx++;
        n->status_lost += (seq - n->status_seq) - 1u;
    }
    n->status_seq = seq;
}

/* The node's relayed C5 state and health (the decoder NUL-terminates fw). */
static void node_take_status(c6_node_t *n, const fp_node_status_t *s) {
    n->cn_x10 = s->cn_x10; n->states = s->states;
    memcpy(n->levels, s->levels, sizeof n->levels);
    n->passes = s->passes; n->flags = s->flags; n->gain = s->gain;
    n->c5_uptime_s = s->uptime_s; n->heap_min = s->heap_min; n->faults = s->faults;
    n->rearm_x10 = s->rearm_db_x10;
    memcpy(n->fw, s->fw, sizeof n->fw);
}

static void on_msg(const uint8_t mac[6], const fp_msg_t *msg, int8_t rssi) {
    c6_node_t back = {0};      /* a node back online: its CONFIG goes out after the lock */
    bool resync = false;
    switch (msg->type) {
    case FP_MSG_PAIR_REQUEST:
        handle_pair(mac, &msg->pair_request);
        break;
    case FP_MSG_GATE_PASS: {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        c6_node_t *n = find_by_mac(mac);
        uint8_t gate = n ? n->gate_index : msg->gate_pass.gate_index;
        if (n && node_touch(n, rssi)) { back = *n; resync = true; }
        xSemaphoreGive(s_lock);
        record_pass(gate, msg->gate_pass.channel, msg->gate_pass.peak_db_x10, msg->gate_pass.fleet_ms);
        break;
    }
    case FP_MSG_NODE_STATUS: {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        c6_node_t *n = find_by_mac(mac);
        if (n) { node_take_status(n, &msg->node_status);
                 if (node_touch(n, rssi)) { back = *n; resync = true; }
                 status_track_loss(n, msg->node_status.seq); }
        xSemaphoreGive(s_lock);
        break;
    }
    /* A node heartbeats only while it boots (c3_fleet_try_online, until it hears us): answer each
     * with its CONFIG, even if it is still online here. Back within NODE_LIVE_MS, it would
     * otherwise keep the settings of before an Apply it missed while it rebooted. */
    case FP_MSG_HEARTBEAT: {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        c6_node_t *n = find_by_mac(mac);
        if (n) { (void)node_touch(n, rssi); n->calib_ms = 0; back = *n; resync = true; }   /* it rebooted: no session */
        xSemaphoreGive(s_lock);
        break;
    }
    case FP_MSG_CALIB_STATE: {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        c6_node_t *n = find_by_mac(mac);
        if (n) { (void)node_touch(n, rssi); n->calib = msg->calib; n->calib_ms = now_ms() | 1u; }
        xSemaphoreGive(s_lock);
        break;
    }
    case FP_MSG_LOG_CHUNK: {
        const fp_log_chunk_t *c = &msg->log_chunk;
        bool again = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_dgate >= 0 && memcmp(mac, s_dmac, 6) == 0) {
            s_dinflight = false;
            if (!(c->flags & FP_LOG_DEBUG)) {
                /* Off at the node: it rebooted, or our DEBUG on is not through yet. */
                if (now_ms() - s_don_ms < DBG_ON_GRACE_MS) again = true;
                else { s_dgate = -1; ESP_LOGW(TAG, "gate debug: the node is out of debug mode, released"); }
            } else {
                uint8_t skip;
                uint32_t gap;
                if (fp_log_take(&s_dexpected, c, &skip, &gap)) {
                    if (gap) dbg_note_lost((unsigned long)gap);
                    lp_text_t t = { .len = (uint8_t)(c->len - skip) };
                    memcpy(t.text, c->text + skip, t.len);
                    lp_logring_put(&s_dring, &t);
                }
                s_dfast = c->len == LP_TEXT_MAX;
            }
        }
        xSemaphoreGive(s_lock);
        if (again) send_debug_frame(mac, true);
        break;
    }
    default:
        break;
    }
    if (resync) {
        ESP_LOGI(TAG, "node id=%u back online or booting: re-sending gate=%u ch=%u", back.node_id,
                 back.gate_index, back.channel);
        send_config_to(&back);
    }
}

static void beacon_task(void *arg) {
    (void)arg;
    for (;;) {
        const fp_time_beacon_t b = { .seq = s_beacon_seq++, .fleet_ms = now_ms() };
        uint8_t buf[FP_MAX_FRAME];
        size_t m = fp_encode_time_beacon(buf, sizeof buf, &b);
        if (m) espnow_link_send(espnow_link_broadcast_addr(), buf, m);

        const uint32_t t = now_ms();
        bool offline = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (unsigned i = 0; i < s_num_nodes; i++)
            if (s_nodes[i].online && t - s_nodes[i].last_seen_ms > NODE_LIVE_MS)
                s_nodes[i].online = false;
        for (unsigned i = 0; i < s_num_nodes; i++)
            if (!s_nodes[i].online) offline = true;
        const uint8_t num = s_num_nodes;
        const bool pairing = pairing_open();
        xSemaphoreGive(s_lock);

        /* Status LED: magenta while pairing with no gates yet, blue if a paired
         * gate dropped offline, green otherwise. */
        status_led_set_base(num == 0u ? (pairing ? SLED_PAIRING : SLED_HEALTHY)
                                      : (offline ? SLED_DEGRADED : SLED_HEALTHY));

        vTaskDelay(pdMS_TO_TICKS(BEACON_MS));
    }
}

static void dbg_task(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(DBG_TICK_MS));
        uint8_t mac[6] = { 0 }, buf[FP_MAX_FRAME];
        size_t n = 0;
        bool lease_over = false;
        const uint32_t t = now_ms();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_dgate >= 0) {
            memcpy(mac, s_dmac, 6);
            if (t - s_dpage_ms > DBG_LEASE_MS) { lease_over = true; s_dgate = -1; }
            else if (s_dinflight ? t - s_dreq_ms >= DBG_REPLY_MS
                                 : (s_dfast || t - s_dreq_ms >= DBG_IDLE_MS)) {
                s_dinflight = true; s_dfast = false; s_dreq_ms = t;
                n = fp_encode_log_req(buf, sizeof buf, s_dexpected);
            }
        }
        xSemaphoreGive(s_lock);
        if (lease_over) { send_debug_frame(mac, false); ESP_LOGW(TAG, "gate debug: no page for a while, off"); }
        else if (n) espnow_link_send(mac, buf, n);
    }
}

void c6_hub_start(void) {
    s_lock = xSemaphoreCreateMutex();
    audio_start();
    volume_apply_saved();
#if CONFIG_C6_BOOT_CHIME
    audio_player_play(AUDIO_SOUND_COIN, audio_pitch_for_channel(0));  /* speaker check */
#endif
    nodes_load();
    nodes_unique_ids();
    ovr_load();
    rt_init(&s_rt, &(rt_config_t){ .num_gates = num_gates() });

    esp_err_t err = espnow_link_start(WIFI_IF_AP, CONFIG_C6_AP_CHANNEL, on_msg);
    if (err != ESP_OK) { ESP_LOGE(TAG, "espnow start: %s", esp_err_to_name(err)); return; }
    for (unsigned i = 0; i < s_num_nodes; i++) espnow_link_add_peer(s_nodes[i].mac);

    xTaskCreate(beacon_task, "c6_beacon", 3072, NULL, 4, NULL);
    xTaskCreate(dbg_task, "c6_dbg", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "fleet hub: %u node(s), %u gate(s), pairing closed (the admin web opens it for %u min)",
             s_num_nodes, num_gates(), PAIRING_OPEN_MS / 60000u);
}

void c6_hub_local_pass(uint8_t channel, int32_t peak_db_x10, uint32_t at_ms) {
    /* Gate 0 = master's local C5, stamped on its own clock: on the fleet clock, like the
     * remote gates, or a sector between it and them means nothing. Only the link task calls. */
    static rt_clock_t local_clock;
    record_pass(0, channel, peak_db_x10, rt_clock_to_fleet(&local_clock, at_ms, now_ms()));
}

void c6_hub_set_pairing(bool open) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_pairing = open;
    s_pairing_until = now_ms() + PAIRING_OPEN_MS;
    xSemaphoreGive(s_lock);
}

bool c6_hub_set_order(const uint8_t *ids, unsigned n) {
    int at[C6_MAX_NODES];                         /* the table index of each id, in the order asked */
    struct { uint8_t mac[6], gate, channel; } to[C6_MAX_NODES];
    unsigned m = 0;
    bool ok = n == s_num_nodes && n <= C6_MAX_NODES, changed = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ok = ok && n == s_num_nodes;                  /* a node paired or removed since the page's list */
    for (unsigned i = 0; ok && i < n; i++) {
        at[i] = -1;
        for (unsigned k = 0; k < s_num_nodes; k++) if (s_nodes[k].node_id == ids[i]) at[i] = (int)k;
        for (unsigned j = 0; j < i; j++) if (at[j] == at[i]) ok = false;   /* an id twice */
        if (at[i] < 0) ok = false;
    }
    if (!ok) { xSemaphoreGive(s_lock); return false; }
    for (unsigned i = 0; i < n; i++) {            /* gates 1..N in the order asked */
        c6_node_t *nd = &s_nodes[at[i]];
        const uint8_t gate = (uint8_t)(i + 1u);
        if (nd->gate_index == gate) continue;
        nd->gate_index = gate;
        changed = true;
        if (nd->online) {
            memcpy(to[m].mac, nd->mac, 6); to[m].gate = gate; to[m].channel = nd->channel;
            m++;
        }
    }
    if (changed) {
        nodes_save();
        rt_init(&s_rt, &(rt_config_t){ .num_gates = num_gates() });   /* the sectors are not the same gates any more */
        if (s_dgate >= 0) {                       /* the debug target is a node: it follows it to its new gate */
            s_dgate = -1;
            for (unsigned k = 0; k < s_num_nodes; k++)
                if (memcmp(s_nodes[k].mac, s_dmac, 6) == 0) s_dgate = s_nodes[k].gate_index;
        }
    }
    xSemaphoreGive(s_lock);
    for (unsigned i = 0; i < m; i++) send_config(to[i].mac, to[i].gate, to[i].channel);   /* offline: when back */
    if (changed) ESP_LOGI(TAG, "gate order changed: %u node(s) renumbered, race timing restarted", m);
    return true;
}

bool c6_hub_set_rearm(int32_t x10) {
    if (!c6_store_set_rearm_x10(x10)) return false;   /* saved: default_config() carries it from now on */
    struct { uint8_t mac[6], gate, channel; } to[C6_MAX_NODES];   /* small: the web task's stack */
    unsigned n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (unsigned i = 0; i < s_num_nodes; i++)
        if (s_nodes[i].online) {
            memcpy(to[n].mac, s_nodes[i].mac, 6);
            to[n].gate = s_nodes[i].gate_index;
            to[n].channel = s_nodes[i].channel;
            n++;
        }
    xSemaphoreGive(s_lock);
    for (unsigned i = 0; i < n; i++) send_config(to[i].mac, to[i].gate, to[i].channel);   /* offline: when back */
    c6_link_sync_rearm();
    ESP_LOGI(TAG, "fleet re-arm drop %ld (x10): sent to %u online node(s) and gate 0", (long)x10, n);
    return true;
}

bool c6_hub_remove_node(uint8_t node_id) {
    uint8_t mac[6];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = -1;
    for (unsigned i = 0; i < s_num_nodes; i++)
        if (s_nodes[i].node_id == node_id) { idx = (int)i; break; }
    if (idx < 0) { xSemaphoreGive(s_lock); return false; }
    memcpy(mac, s_nodes[idx].mac, 6);
    (void)ovr_drop(mac);
    for (unsigned i = (unsigned)idx; i + 1u < s_num_nodes; i++) s_nodes[i] = s_nodes[i + 1u];
    s_num_nodes--;
    nodes_save();
    rt_init(&s_rt, &(rt_config_t){ .num_gates = num_gates() });
    xSemaphoreGive(s_lock);
    espnow_link_del_peer(mac);
    return true;
}

bool c6_hub_reset_channel(uint8_t channel) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool ok = rt_reset_channel(&s_rt, channel);
    xSemaphoreGive(s_lock);
    return ok;
}

void c6_hub_results(race_timing_t *out) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_rt;
    xSemaphoreGive(s_lock);
}

void c6_hub_reset_race(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    rt_reset(&s_rt);
    xSemaphoreGive(s_lock);
}

bool c6_hub_send_command(uint8_t gate_index, lp_cmd_t cmd) {
    if (gate_index == 0u) {                    /* the master's own C5, over UART */
        if (!c6_store_has_local()) return false;
        c6_link_send_command(cmd);
        return true;
    }
    /* Normally one node per gate; a gate the admin gave to two nodes reaches both. */
    uint8_t macs[C6_MAX_NODES][6];
    unsigned n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (unsigned i = 0; i < s_num_nodes; i++)
        if (s_nodes[i].gate_index == gate_index && s_nodes[i].online) memcpy(macs[n++], s_nodes[i].mac, 6);
    xSemaphoreGive(s_lock);
    const fp_command_t c = { .gate_index = gate_index, .cmd = (uint8_t)cmd };
    uint8_t buf[FP_MAX_FRAME];
    const size_t m = fp_encode_command(buf, sizeof buf, &c);
    bool sent = false;
    for (unsigned i = 0; m && i < n; i++)
        if (espnow_link_send(macs[i], buf, m) == ESP_OK) sent = true;
    if (sent) ESP_LOGI(TAG, "command %d -> gate %u", (int)cmd, gate_index);
    return sent;
}

bool c6_hub_send_gain(uint8_t gate_index, uint8_t gain) {
    if (gain < LP_GAIN_MIN || gain > LP_GAIN_MAX) return false;
    if (gate_index == 0u) {
        if (!c6_store_has_local()) return false;
        c6_link_send_gain(gain);
        return true;
    }
    uint8_t macs[C6_MAX_NODES][6];
    unsigned n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (unsigned i = 0; i < s_num_nodes; i++)
        if (s_nodes[i].gate_index == gate_index && s_nodes[i].online) memcpy(macs[n++], s_nodes[i].mac, 6);
    xSemaphoreGive(s_lock);
    const fp_gain_t g = { .gate_index = gate_index, .gain = gain };
    uint8_t buf[FP_MAX_FRAME];
    const size_t m = fp_encode_gain(buf, sizeof buf, &g);
    bool sent = false;
    for (unsigned i = 0; m && i < n; i++)
        if (espnow_link_send(macs[i], buf, m) == ESP_OK) sent = true;
    if (sent) ESP_LOGI(TAG, "gain %u -> gate %u", gain, gate_index);
    return sent;
}

/* The gate's online nodes' MACs; s_lock taken inside. */
static unsigned gate_macs(uint8_t gate, uint8_t macs[][6]) {
    unsigned n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (unsigned i = 0; i < s_num_nodes; i++)
        if (s_nodes[i].gate_index == gate && s_nodes[i].online) memcpy(macs[n++], s_nodes[i].mac, 6);
    xSemaphoreGive(s_lock);
    return n;
}

bool c6_hub_calib_start(uint8_t gate_index, uint8_t step, uint8_t channel) {
    if (step > 2u) return false;
    if (gate_index == 0u) {
        if (!c6_store_has_local()) return false;
        c6_link_calib_start(step, channel);
        return true;
    }
    uint8_t macs[C6_MAX_NODES][6];
    const unsigned n = gate_macs(gate_index, macs);
    uint8_t buf[FP_MAX_FRAME];
    const size_t m = fp_encode_calib_start(buf, sizeof buf, step, channel);
    bool sent = false;
    for (unsigned i = 0; m && i < n; i++) {
        if (espnow_link_send(macs[i], buf, m) == ESP_OK) sent = true;
        if (step == 0) {                                          /* the page no longer shows it */
            xSemaphoreTake(s_lock, portMAX_DELAY);
            c6_node_t *nd = find_by_mac(macs[i]);
            if (nd) nd->calib_ms = 0;
            xSemaphoreGive(s_lock);
        }
    }
    return sent;
}

bool c6_hub_calib_get(uint8_t gate_index, lp_calib_t *out) {
    memset(out, 0, sizeof *out);
    if (gate_index == 0u) return c6_link_calib_get(out);
    bool have = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (unsigned i = 0; i < s_num_nodes && !have; i++) {
        const c6_node_t *n = &s_nodes[i];
        if (n->gate_index != gate_index || !n->online || !n->calib_ms || n->calib.phase == LP_CALIB_NONE) continue;
        if (now_ms() - n->calib_ms > 4000u) continue;               /* its C5 sends one a second: this one is old */
        *out = n->calib;
        have = true;
    }
    xSemaphoreGive(s_lock);
    return have;
}

bool c6_hub_calib_apply(uint8_t gate_index) {
    if (gate_index == 0u) return c6_link_calib_apply();
    lp_calib_t k;
    if (!c6_hub_calib_get(gate_index, &k) || k.phase != LP_CALIB_DONE || k.min_peak_db_x10 <= 0) return false;
    struct { uint8_t mac[6], gate, channel; } to[C6_MAX_NODES];
    unsigned n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (unsigned i = 0; i < s_num_nodes; i++)
        if (s_nodes[i].gate_index == gate_index && s_nodes[i].online) {
            ovr_set(s_nodes[i].mac, &k);
            memcpy(to[n].mac, s_nodes[i].mac, 6); to[n].gate = s_nodes[i].gate_index; to[n].channel = s_nodes[i].channel;
            n++;
        }
    xSemaphoreGive(s_lock);
    for (unsigned i = 0; i < n; i++) send_config(to[i].mac, to[i].gate, to[i].channel);
    (void)c6_hub_calib_start(gate_index, 0, LP_CALIB_ANY);                         /* the proposal is applied: end the session */
    ESP_LOGI(TAG, "gate %u thresholds from its passage calibration: active %d hyst %d drop %d min_peak %d (x10), %u node(s)",
             gate_index, (int)k.active_db_x10, (int)k.hysteresis_db_x10, (int)k.drop_db_x10, (int)k.min_peak_db_x10, n);
    return n > 0;
}

bool c6_hub_calib_reset(uint8_t gate_index) {
    if (gate_index == 0u) return false;
    struct { uint8_t mac[6], gate, channel; } to[C6_MAX_NODES];
    unsigned n = 0;
    bool dropped = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (unsigned i = 0; i < s_num_nodes; i++)
        if (s_nodes[i].gate_index == gate_index) {
            dropped |= ovr_drop(s_nodes[i].mac);
            if (s_nodes[i].online) {
                memcpy(to[n].mac, s_nodes[i].mac, 6); to[n].gate = s_nodes[i].gate_index; to[n].channel = s_nodes[i].channel;
                n++;
            }
        }
    xSemaphoreGive(s_lock);
    for (unsigned i = 0; i < n; i++) send_config(to[i].mac, to[i].gate, to[i].channel);   /* the fleet's again */
    return dropped || n > 0;
}

/* Remote gates run the detector config the hub pushes (at pairing, on a save and
 * each time a node comes back online), so its activity level is where a C/N says
 * a VTX is on. Gate 0 uses its own C5's echoed active_db once known (c6_store). */
int32_t c6_hub_vtx_threshold_x10(void) { return default_config().active_db_x10; }

void c6_hub_snapshot(c6_fleet_t *out) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->pairing = pairing_open();
    out->pairing_left_s = out->pairing ? (s_pairing_until - now_ms() + 999u) / 1000u : 0u;
    out->num_gates = num_gates();
    out->num_nodes = s_num_nodes;
    out->volume = (uint8_t)audio_player_volume();
    out->uptime_ms = now_ms();
    out->free_heap = esp_get_free_heap_size();
    out->heap_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    for (unsigned i = 0; i < s_num_nodes; i++) {
        out->nodes[i] = s_nodes[i];
        out->nodes[i].custom = find_ovr(s_nodes[i].mac) != NULL;
    }
    out->num_standings = s_rt.num_channels;
    for (unsigned i = 0; i < s_rt.num_channels; i++) {
        out->standings[i].channel = s_rt.ch[i].channel;
        out->standings[i].laps = s_rt.ch[i].laps;
        out->standings[i].last_lap_ms = s_rt.ch[i].last_lap_ms;
        out->standings[i].best_lap_ms = s_rt.ch[i].best_lap_ms;
        out->standings[i].last_gate = s_rt.ch[i].last_gate;
        out->standings[i].last_seen_ms = s_rt.ch[i].last_cross_ms;
    }
    xSemaphoreGive(s_lock);
}

/* ---- debug mode of a remote gate (see the block at the top) ---- */

/* The first online node of a gate, false if none. s_lock held. */
static bool dbg_find(uint8_t gate, uint8_t mac[6]) {
    for (unsigned i = 0; i < s_num_nodes; i++)
        if (s_nodes[i].gate_index == gate && s_nodes[i].online) { memcpy(mac, s_nodes[i].mac, 6); return true; }
    return false;
}

bool c6_hub_debug_set(uint8_t gate, bool on) {
    uint8_t mac[6] = { 0 }, old[6] = { 0 };
    bool had = false, ok = true;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_dgate >= 0 && (on ? s_dgate != (int)gate : s_dgate == (int)gate)) {   /* one target at a time: the previous one off */
        memcpy(old, s_dmac, 6);
        had = true;
        s_dgate = -1;
    }
    if (on) {
        if (dbg_find(gate, mac)) {
            const bool same = s_dgate == (int)gate;
            if (!same) {
                lp_logring_reset(&s_dring);
                s_dexpected = 0;
                s_dinflight = s_dfast = false;
                s_dgate = gate;
                memcpy(s_dmac, mac, 6);
                s_don_ms = s_dreq_ms = now_ms();
            }
            s_dpage_ms = now_ms();
        } else ok = false;
    }
    xSemaphoreGive(s_lock);
    if (had) send_debug_frame(old, false);
    if (on && ok) send_debug_frame(mac, true);
    return ok;
}

int c6_hub_debug_gate(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int g = s_dgate;
    xSemaphoreGive(s_lock);
    return g;
}

size_t c6_hub_log_read(uint32_t since, char *out, size_t max, uint32_t *next, bool *lost) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_dpage_ms = now_ms();                       /* the page is still there: renews the lease */
    const size_t n = lp_logring_read(&s_dring, since, out, max, next, lost);
    xSemaphoreGive(s_lock);
    return n;
}

bool c6_hub_send_keys(uint8_t gate, const char *keys, size_t n) {
    uint8_t mac[6] = { 0 }, buf[FP_MAX_FRAME];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool ok = s_dgate == (int)gate && dbg_find(gate, mac);
    xSemaphoreGive(s_lock);
    const size_t m = ok ? fp_encode_key(buf, sizeof buf, keys, n) : 0;
    return m && espnow_link_send(mac, buf, m) == ESP_OK;
}
