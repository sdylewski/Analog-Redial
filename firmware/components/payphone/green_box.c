/*
 * green_box.c -- see green_box.h.
 *
 * A simple state machine driven by a one-shot esp_timer.  Each burst is
 * played with audio_tx_burst(); the timer fires after (burst + gap) to
 * start the next one, or after the last burst finishes to clear the busy
 * flag.
 */
#include "green_box.h"
#include "audio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdatomic.h>

static const char *TAG = "green_box";

static esp_timer_handle_t s_timer;
static atomic_bool        s_busy;
static int                s_line;
static int                s_total;   /* bursts in this sequence */
static int                s_done;    /* bursts started so far */

/* How many bursts for each action. */
static int bursts_for(payphone_relay_t action)
{
    switch (action) {
        case PAYPHONE_COLLECT: return 1;
        case PAYPHONE_RETURN:  return 2;
        default:               return 0;
    }
}

static void play_burst(void)
{
    audio_tx_burst(s_line, GB_F1, GB_F2, GB_AMP, GB_BURST_MS);
    s_done++;
    ESP_LOGD(TAG, "line %d: burst %d/%d", s_line, s_done, s_total);
}

static void timer_cb(void *arg)
{
    (void)arg;
    if (s_done < s_total) {
        /* Play the next burst. */
        play_burst();
        if (s_done < s_total) {
            /* More to come: wait for this burst + gap. */
            esp_timer_start_once(s_timer, (uint64_t)(GB_BURST_MS + GB_GAP_MS) * 1000);
        } else {
            /* Last burst is now playing: wait for it to finish. */
            esp_timer_start_once(s_timer, (uint64_t)GB_BURST_MS * 1000);
        }
    } else {
        /* All bursts have finished playing. */
        ESP_LOGI(TAG, "line %d: sequence done (%d burst%s)", s_line, s_total,
                 s_total == 1 ? "" : "s");
        atomic_store(&s_busy, false);
    }
}

void green_box_init(void)
{
    if (s_timer) return;   /* already initialised */
    esp_timer_create_args_t args = {
        .callback = timer_cb,
        .name = "green_box",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_timer));
    atomic_store(&s_busy, false);
    ESP_LOGI(TAG, "green box tone generator ready");
}

esp_err_t green_box_fire(int line, payphone_relay_t action)
{
    int n = bursts_for(action);
    if (n == 0) return ESP_ERR_INVALID_ARG;

    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_busy, &expected, true)) {
        ESP_LOGW(TAG, "line %d: busy, dropping %s", line,
                 action == PAYPHONE_COLLECT ? "collect" : "return");
        return ESP_ERR_INVALID_STATE;
    }

    s_line  = line;
    s_total = n;
    s_done  = 0;

    ESP_LOGI(TAG, "line %d: %s (%d burst%s of %.0f+%.0f Hz, %d ms)",
             line, action == PAYPHONE_COLLECT ? "collect" : "return",
             n, n == 1 ? "" : "s", GB_F1, GB_F2, GB_BURST_MS);

    /* First burst immediately. */
    play_burst();

    if (s_total > 1) {
        /* Schedule the next burst after this one + the gap. */
        esp_timer_start_once(s_timer, (uint64_t)(GB_BURST_MS + GB_GAP_MS) * 1000);
    } else {
        /* Single burst (collect): wait for it to finish, then clear busy. */
        esp_timer_start_once(s_timer, (uint64_t)GB_BURST_MS * 1000);
    }
    return ESP_OK;
}

bool green_box_busy(void)
{
    return atomic_load(&s_busy);
}
