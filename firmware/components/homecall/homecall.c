/* homecall.c -- see homecall.h. */
#include "homecall.h"

#include "board.h"
#include "dialplan.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"
#include "voip.h"
#include "wifi.h"
#include <stdio.h>
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
    mdns_txt_item_t txt[2];
    size_t n = 0;
    if (s_num[0][0]) txt[n++] = (mdns_txt_item_t){ "n0", s_num[0] };
    if (s_num[1][0]) txt[n++] = (mdns_txt_item_t){ "n1", s_num[1] };
    bool want = s_num[0][0] || s_num[1][0];
    bool have = mdns_service_exists(SERVICE, PROTO, NULL);
    if (!want) {
        if (have) mdns_service_remove(SERVICE, PROTO);
        s_advertised = false;
        return;
    }
    esp_err_t err = have ? mdns_service_txt_set(SERVICE, PROTO, txt, n)
                         : mdns_service_add(NULL, SERVICE, PROTO, 5060, txt, n);
    if (err == ESP_OK) s_advertised = true;
    else ESP_LOGW(TAG, "mDNS advertise: %s", esp_err_to_name(err));
}

const char *homecall_number(int line)
{
    return (line >= 0 && line < 2) ? s_num[line] : "";
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

void homecall_refresh(void)
{
    if (!wifi_is_connected()) return;
    browse();
    push_numbers();
}

static void homecall_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (wifi_is_connected()) {
            if (!s_advertised && (s_num[0][0] || s_num[1][0])) advertise();
            browse();
            push_numbers();
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BROWSE_EVERY_MS));
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
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) { snprintf(msg, msg_len, "Couldn't save (%s)", esp_err_to_name(err)); return err; }
    strlcpy(s_num[line], num, sizeof(s_num[line]));
    s_advertised = false;
    advertise();
    push_numbers();
    if (num[0]) snprintf(msg, msg_len, "Line %d is now %s on your home network", line + 1, num);
    else snprintf(msg, msg_len, "Line %d has no home number now", line + 1);
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
        nvs_close(h);
    }
    push_numbers();
    xTaskCreate(homecall_task, "homecall", 4096, NULL, tskIDLE_PRIORITY + 1, &s_task);
    if (s_num[0][0] || s_num[1][0]) ESP_LOGI(TAG, "home numbers: '%s' / '%s'", s_num[0], s_num[1]);
}
