/*
 * voip.c — STUB implementation of the VoIP backend.
 *
 * Milestone 5 replaces this with an embedded SIP (e.g. a port of a small SIP
 * stack or esp_sip) + RTP/G.711 over Wi-Fi, registering to the external
 * Asterisk PBX (see ../pi_rotary_phone/docs/VOIP_SETUP.md for the topology).
 *
 * The stub keeps the rest of the firmware buildable and testable: the
 * telephony FSM can call voip_dial()/answer()/hangup() and handle the
 * NOT_SUPPORTED result (falls back to reorder tone), and a future real
 * backend drops in without FSM changes.
 */
#include "voip.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "voip";

static voip_event_cb s_cb;
static void         *s_ctx;
static bool          s_registered[2];

esp_err_t voip_init(voip_event_cb cb, void *ctx)
{
    s_cb = cb;
    s_ctx = ctx;
    ESP_LOGW(TAG, "STUB VoIP backend — SIP/RTP not compiled in (milestone 5)");
    return ESP_OK;
}

esp_err_t voip_register(const voip_account_t *acct)
{
    if (!acct) return ESP_ERR_INVALID_ARG;
    ESP_LOGW(TAG, "register(line=%d user=%s domain=%s): stub, not registering",
             acct->line, acct->user ? acct->user : "?",
             acct->domain ? acct->domain : "?");
    if (acct->line >= 0 && acct->line < 2) s_registered[acct->line] = false;
    if (s_cb) {
        voip_event_t ev = { .line = acct->line, .type = VOIP_EV_REGISTER_FAILED };
        s_cb(&ev, s_ctx);
    }
    return ESP_ERR_NOT_SUPPORTED;
}

bool voip_is_registered(int line)
{
    return (line >= 0 && line < 2) ? s_registered[line] : false;
}

esp_err_t voip_dial(int line, const char *number)
{
    ESP_LOGW(TAG, "dial(line=%d, %s): stub", line, number ? number : "?");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t voip_answer(int line)
{
    ESP_LOGW(TAG, "answer(line=%d): stub", line);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t voip_hangup(int line)
{
    ESP_LOGD(TAG, "hangup(line=%d): stub", line);
    return ESP_OK;
}

bool voip_is_stub(void) { return true; }
