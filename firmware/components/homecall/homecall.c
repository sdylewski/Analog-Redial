/* homecall.c -- see homecall.h. */
#include "homecall.h"

#include "board.h"
#include "dialplan.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"
#include "voip.h"
#include "wifi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "homecall";

#define NVS_NS       "homecall"
#define SERVICE      "_redial-phone"
#define PROTO        "_udp"
#define BROWSE_EVERY_MS  60000
#define PEER_EXPIRE_S    300     /* gone this long: drop it */

typedef struct {
    homecall_peer_t p;
    int64_t seen_us;
} peer_slot_t;

static char              s_num[2][HOMECALL_NUM_MAX];
static peer_slot_t       s_peers[HOMECALL_PEERS_MAX];
static size_t            s_npeers;
static SemaphoreHandle_t s_lock;
static TaskHandle_t      s_task;
static bool              s_advertised;
static bool              s_seeded;   /* numbers were chosen (by the board or by hand) at least once */
static bool              s_auto;     /* ...and the board chose them */


/* ---- numbers ---------------------------------------------------------- */

static void push_numbers(void)
{
    voip_set_home_number(0, s_num[0]);
    voip_set_home_number(1, s_num[1]);
    dialplan_set_own_home_numbers(s_num[0], s_num[1]);

    char list[DIALPLAN_HOME_MAX][DIALPLAN_HOME_NUM_MAX];
    size_t n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < s_npeers; i++) {
        for (int l = 0; l < 2; l++) {
            const char *num = s_peers[i].p.number[l];
            if (num[0] && n < DIALPLAN_HOME_MAX) strlcpy(list[n++], num, DIALPLAN_HOME_NUM_MAX);
        }
    }
    xSemaphoreGive(s_lock);
    dialplan_set_home_numbers((const char (*)[DIALPLAN_HOME_NUM_MAX])list, n);
}

static void advertise(void)
{
    /* Advertised even with no number, so other boards list this one as "not
     * set up" rather than not seeing it at all. */
    mdns_txt_item_t txt[2];
    size_t n = 0;
    if (s_num[0][0]) txt[n++] = (mdns_txt_item_t){ "n0", s_num[0] };
    if (s_num[1][0]) txt[n++] = (mdns_txt_item_t){ "n1", s_num[1] };
    bool have = mdns_service_exists(SERVICE, PROTO, NULL);
    esp_err_t err = have ? mdns_service_txt_set(SERVICE, PROTO, txt, n)
                         : mdns_service_add(NULL, SERVICE, PROTO, 5060, n ? txt : NULL, n);
    if (err == ESP_OK) s_advertised = true;
    else ESP_LOGW(TAG, "mDNS advertise: %s", esp_err_to_name(err));
}

const char *homecall_number(int line)
{
    return (line >= 0 && line < 2) ? s_num[line] : "";
}

bool homecall_number_is_automatic(void)
{
    return s_auto;
}

/* ---- discovery -------------------------------------------------------- */

static void browse(void)
{
    char myip[16];
    wifi_get_ip_str(myip, sizeof(myip));
    mdns_result_t *res = NULL;
    if (mdns_query_ptr(SERVICE, PROTO, 3000, 20, &res) != ESP_OK) return;
    int64_t now = esp_timer_get_time();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (mdns_result_t *r = res; r; r = r->next) {
        char ip[16] = "";
        for (mdns_ip_addr_t *a = r->addr; a; a = a->next) {
            if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                snprintf(ip, sizeof(ip), IPSTR, IP2STR(&a->addr.u_addr.ip4));
                break;
            }
        }
        if (!ip[0] || strcmp(ip, myip) == 0) continue;   /* no address yet, or ourselves */
        peer_slot_t *slot = NULL;
        for (size_t i = 0; i < s_npeers; i++) if (strcmp(s_peers[i].p.ip, ip) == 0) slot = &s_peers[i];
        if (!slot && s_npeers < HOMECALL_PEERS_MAX) slot = &s_peers[s_npeers++];
        if (!slot) continue;
        memset(&slot->p, 0, sizeof(slot->p));
        strlcpy(slot->p.ip, ip, sizeof(slot->p.ip));
        strlcpy(slot->p.name, r->instance_name ? r->instance_name : (r->hostname ? r->hostname : ip),
                sizeof(slot->p.name));
        for (size_t t = 0; t < r->txt_count; t++) {
            const char *k = r->txt[t].key, *v = r->txt[t].value;
            if (!k || !v) continue;
            int line = !strcmp(k, "n0") ? 0 : !strcmp(k, "n1") ? 1 : -1;
            if (line >= 0) strlcpy(slot->p.number[line], v, HOMECALL_NUM_MAX);
        }
        slot->seen_us = now;
    }
    /* Forget boards that stopped answering. */
    for (size_t i = 0; i < s_npeers; ) {
        if (now - s_peers[i].seen_us > (int64_t)PEER_EXPIRE_S * 1000000) {
            s_peers[i] = s_peers[--s_npeers];
        } else {
            i++;
        }
    }
    xSemaphoreGive(s_lock);
    mdns_query_results_free(res);
}

/* ---- first-start numbers -------------------------------------------------- */

static esp_err_t save_number(int line, const char *num, bool automatic)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, line ? "n1" : "n0", num);
    if (err == ESP_OK) err = nvs_set_u8(h, "auto", automatic ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, "seeded", 1);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* A free number for `line`: 4 digits, leading 1 where allowed (1 belongs to
 * Bluetooth dial-out when a module is fitted), valid for the dial plan, and
 * not in use by this board's other line or any board seen so far. */
static bool pick_number(int line, const char *prefer, char *out)
{
    bool bt = board_hw_config_get().has_bt_module;
    char cand[HOMECALL_NUM_MAX];
    for (int attempt = 0; attempt < 300; attempt++) {
        if (attempt == 0 && prefer && prefer[0]) {
            strlcpy(cand, prefer, sizeof(cand));
        } else {
            uint32_t r = esp_random();
            int first = (!bt && attempt < 60) ? 1 : (bt ? 2 : 1) + (int)((r >> 24) % (bt ? 8 : 9));
            snprintf(cand, sizeof(cand), "%d%03u", first, (unsigned)(r % 1000));
        }
        if (dialplan_home_number_problem(cand)) continue;
        if (!strcmp(cand, s_num[line ^ 1])) continue;
        if (homecall_lookup(cand, NULL, 0, NULL, 0)) continue;
        strlcpy(out, cand, HOMECALL_NUM_MAX);
        return true;
    }
    return false;
}

static void next_in_sequence(const char *num, char *out)
{
    long v = atol(num) + 1;
    snprintf(out, HOMECALL_NUM_MAX, "%ld", v);
}

/* Picks this board's numbers the first time it is on a network. */
static void seed_numbers(void)
{
    int lines = board_hw_config_get().num_lines > 1 ? 2 : 1;
    char num[HOMECALL_NUM_MAX], seq[HOMECALL_NUM_MAX];
    for (int l = 0; l < lines; l++) {
        const char *prefer = NULL;
        if (l == 1 && s_num[0][0]) { next_in_sequence(s_num[0], seq); prefer = seq; }
        if (!pick_number(l, prefer, num)) continue;
        if (save_number(l, num, true) == ESP_OK) {
            strlcpy(s_num[l], num, sizeof(s_num[l]));
            ESP_LOGI(TAG, "line %d: first-start number %s", l + 1, num);
        }
    }
    s_seeded = true;
    s_auto = true;
    s_advertised = false;
    advertise();
    push_numbers();
}

/* Two new boards can pick the same number before they see each other. A
 * board that chose its own number picks again; numbers set by hand stay. */
static void settle_clashes(void)
{
    if (!s_auto) return;
    for (int l = 0; l < 2; l++) {
        if (!s_num[l][0]) continue;
        char ip[16];
        if (!homecall_lookup(s_num[l], ip, sizeof(ip), NULL, 0)) continue;
        char num[HOMECALL_NUM_MAX];
        s_num[l][0] = '\0';
        if (pick_number(l, NULL, num) && save_number(l, num, true) == ESP_OK) {
            strlcpy(s_num[l], num, sizeof(s_num[l]));
            ESP_LOGI(TAG, "line %d: number was taken by %s, now %s", l + 1, ip, num);
        }
        s_advertised = false;
        advertise();
        push_numbers();
    }
}

void homecall_refresh(void)
{
    if (!wifi_is_connected()) return;
    browse();
    push_numbers();
}

/* Boards seen last time, kept across reboots, so a number dialled right after
 * power-up already works. Written only when the list changes. */
static homecall_peer_t s_cache_saved[HOMECALL_PEERS_MAX];
static size_t          s_cache_saved_n;

static void cache_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = sizeof(s_cache_saved);
    if (nvs_get_blob(h, "peers", s_cache_saved, &sz) == ESP_OK && sz % sizeof(homecall_peer_t) == 0) {
        s_cache_saved_n = sz / sizeof(homecall_peer_t);
        int64_t now = esp_timer_get_time();
        for (size_t i = 0; i < s_cache_saved_n && s_npeers < HOMECALL_PEERS_MAX; i++) {
            s_peers[s_npeers].p = s_cache_saved[i];
            s_peers[s_npeers].p.age_s = 0;
            s_peers[s_npeers].seen_us = now;   /* expires in PEER_EXPIRE_S unless found again */
            s_npeers++;
        }
    } else {
        s_cache_saved_n = 0;
    }
    nvs_close(h);
}

static void cache_save_if_changed(void)
{
    homecall_peer_t cur[HOMECALL_PEERS_MAX];
    memset(cur, 0, sizeof(cur));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = s_npeers;
    for (size_t i = 0; i < n; i++) { cur[i] = s_peers[i].p; cur[i].age_s = 0; }
    xSemaphoreGive(s_lock);
    if (n == s_cache_saved_n && memcmp(cur, s_cache_saved, n * sizeof(cur[0])) == 0) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t err = n ? nvs_set_blob(h, "peers", cur, n * sizeof(cur[0])) : nvs_erase_key(h, "peers");
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        memcpy(s_cache_saved, cur, sizeof(cur));
        s_cache_saved_n = n;
    }
}

static void homecall_task(void *arg)
{
    (void)arg;
    int pass = 0;   /* passes since Wi-Fi came up */
    for (;;) {
        uint32_t wait_ms = BROWSE_EVERY_MS;
        if (!wifi_is_connected()) {
            pass = 0;
            wait_ms = 1000;
        } else {
            char myip[16];
            wifi_get_ip_str(myip, sizeof(myip));
            if (pass == 0) vTaskDelay(pdMS_TO_TICKS(2000));   /* let this board's own mDNS name settle */
            if (!s_advertised) advertise();
            browse();
            if (!s_seeded) seed_numbers();
            settle_clashes();
            push_numbers();
            cache_save_if_changed();
            /* Look quickly for the first minute after Wi-Fi comes up: other boards
             * answer slowly, and a number dialled just after power-up has to work. */
            if (++pass < 6) wait_ms = 8000;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }
}

size_t homecall_peers(homecall_peer_t *out, size_t max)
{
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = s_npeers < max ? s_npeers : max;
    for (size_t i = 0; i < n; i++) {
        out[i] = s_peers[i].p;
        out[i].age_s = (uint32_t)((now - s_peers[i].seen_us) / 1000000);
    }
    xSemaphoreGive(s_lock);
    return n;
}

bool homecall_lookup(const char *number, char *ip, size_t ip_len, char *name, size_t name_len)
{
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < s_npeers && !found; i++) {
        for (int l = 0; l < 2; l++) {
            if (s_peers[i].p.number[l][0] && strcmp(s_peers[i].p.number[l], number) == 0) {
                if (ip) strlcpy(ip, s_peers[i].p.ip, ip_len);
                if (name) strlcpy(name, s_peers[i].p.name, name_len);
                found = true;
                break;
            }
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

/* ---- setting a number --------------------------------------------------- */

esp_err_t homecall_set_number(int line, const char *number, char *msg, size_t msg_len)
{
    if (line < 0 || line > 1) { snprintf(msg, msg_len, "No such line"); return ESP_ERR_INVALID_ARG; }
    char num[HOMECALL_NUM_MAX] = "";
    if (number) {
        size_t o = 0;   /* allow spaces and dashes when typed: 12-34 */
        for (const char *p = number; *p && o < sizeof(num) - 1; p++) {
            if (*p == ' ' || *p == '-') continue;
            num[o++] = *p;
        }
        num[o] = '\0';
    }
    if (num[0]) {
        const char *why = dialplan_home_number_problem(num);
        if (why) { snprintf(msg, msg_len, "%s", why); return ESP_ERR_INVALID_ARG; }
        if (strcmp(num, s_num[line ^ 1]) == 0) {
            snprintf(msg, msg_len, "The other line already has %s", num);
            return ESP_ERR_INVALID_STATE;
        }
        homecall_refresh();   /* look again right now, so a clash isn't missed */
        char ip[16], name[32];
        if (homecall_lookup(num, ip, sizeof(ip), name, sizeof(name))) {
            snprintf(msg, msg_len, "%s is already %s's number (%s)", num, name, ip);
            return ESP_ERR_INVALID_STATE;
        }
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, line ? "n1" : "n0", num);
        if (err == ESP_OK) err = nvs_set_u8(h, "auto", 0);
        if (err == ESP_OK) err = nvs_set_u8(h, "seeded", 1);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) { snprintf(msg, msg_len, "Couldn't save (%s)", esp_err_to_name(err)); return err; }
    strlcpy(s_num[line], num, sizeof(s_num[line]));
    s_seeded = true;
    s_auto = false;
    s_advertised = false;
    advertise();
    push_numbers();
    if (num[0]) snprintf(msg, msg_len, "Line %d is now %s for board-to-board calls", line + 1, num);
    else snprintf(msg, msg_len, "Line %d has no board-to-board number now", line + 1);
    ESP_LOGI(TAG, "%s", msg);
    return ESP_OK;
}

void homecall_init(void)
{
    if (s_lock) return;
    s_lock = xSemaphoreCreateMutex();
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(s_num[0]); nvs_get_str(h, "n0", s_num[0], &sz);
        sz = sizeof(s_num[1]);        nvs_get_str(h, "n1", s_num[1], &sz);
        uint8_t v = 0;
        if (nvs_get_u8(h, "seeded", &v) == ESP_OK) s_seeded = v != 0;
        if (nvs_get_u8(h, "auto", &v) == ESP_OK) s_auto = v != 0;
        nvs_close(h);
    }
    cache_load();
    /* A board that already had a number before first-start numbers existed. */
    if (s_num[0][0] || s_num[1][0]) s_seeded = true;
    push_numbers();
    xTaskCreate(homecall_task, "homecall", 4096, NULL, tskIDLE_PRIORITY + 1, &s_task);
    if (s_num[0][0] || s_num[1][0]) ESP_LOGI(TAG, "home numbers: '%s' / '%s'", s_num[0], s_num[1]);
}
