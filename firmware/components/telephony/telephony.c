#include "telephony.h"
#include "dialplan.h"
#include "ag1170.h"
#include "board_pins.h"   /* BOARD_NUM_LINES */
#include "audio.h"
#include "dtmf.h"
#include "tones.h"
#include "voip.h"
#include "player.h"
#include "recorder.h"
#include "storage.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "telephony";

/* SNTP (see wifi.c) sets the system clock without a TZ, so this is UTC.
 * Before it syncs, time() reads from the 1970 epoch -- anything before
 * ~2024 is obviously unsynced, so fall back to a boot-relative tag rather
 * than naming every early recording "19700101_000003.wav" and colliding. */
static void rec_name_timestamped(char *out, size_t out_len, int line)
{
    time_t now = time(NULL);
    if (now < 1700000000) {
        snprintf(out, out_len, "rec_l%d_boot%lld.wav", line,
                 (long long)(esp_timer_get_time() / 1000));
        return;
    }
    struct tm t;
    gmtime_r(&now, &t);
    snprintf(out, out_len, "rec_l%d_%04d%02d%02d_%02d%02d%02dZ.wav", line,
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
}

/* Array width stays 2 to match the codec's stereo L/R pair (AUDIO_NUM_LINES),
 * so per-line state is never indexed out of bounds. But only BOARD_NUM_LINES
 * lines are actually *serviced* -- on a 1-line board, line 1 has no SLIC, so
 * feeding it hook/ring/VoIP events would leave a phantom line stuck (e.g. an
 * inbound VoIP call to the 2nd extension "ringing" a handset that isn't
 * there). Events for lines >= BOARD_NUM_LINES are dropped, and the periodic
 * FSM servicing/taps only run for real lines. */
#define NLINES        2
#define NLINES_ACTIVE BOARD_NUM_LINES
#define PEER_NONE     (-1)
#define PEER_VOIP     (-2)

/* internal event queue (decouples ISR/other-task callbacks from the FSM) */
typedef enum {
    IN_OFFHOOK = 0, IN_ONHOOK, IN_DIGIT,
    IN_VOIP_INCOMING, IN_VOIP_ANSWERED, IN_VOIP_CLOSED,
    IN_CLIP_DONE,
    IN_REC_DONE,
} in_type_t;

typedef struct {
    int       line;
    in_type_t type;
    char      digit;
} in_evt_t;

typedef struct {
    tel_state_t          state;
    dialplan_collector_t coll;
    bool                 dialing;
    int64_t              last_digit_us;
    int                  peer;          /* local peer line, PEER_VOIP, or PEER_NONE */
    voice_effect_t       call_fx;
    dtmf_detector_t      det;
    char                 last_number[16];
    /* Caller ID of the most recent inbound VoIP call (kept after the call
     * ends so the web UI can show who last called). caller_name is "" when
     * the caller sent no display name. */
    char                 caller_name[40];
    char                 caller_number[32];
    char                 last_dtmf_digit;   /* for tonemon/bringup introspection */
    int64_t              last_dtmf_us;
    /* transient tone auto-return */
    int64_t              tone_return_us; /* 0 = no auto-return */
    tel_state_t          tone_return_state;
    /* active recording (211 archive) */
    char                 rec_name[STORAGE_MAX_NAME];
} line_t;

static line_t        s_line[NLINES];
static QueueHandle_t s_q;

/* Test-mode injection (host DVT harness). When enabled, off-hook/on-hook and
 * dialed digits can be injected via telephony_test_*(); s_test_offhook shadows
 * the real SLIC hook state so the FSM's hook-dependent branches (post-clip
 * return to dial tone vs. idle, etc.) behave exactly as with a real handset. */
static volatile bool s_test_enabled;
static bool          s_test_offhook[NLINES];

/* Effective hook state: injected shadow in test mode, else the real SLIC. */
static bool line_offhook(int line)
{
    if (s_test_enabled) return (line >= 0 && line < NLINES) ? s_test_offhook[line] : false;
    return ag1170_is_offhook(line);
}

const char *telephony_state_name(tel_state_t s)
{
    switch (s) {
        case TEL_IDLE: return "IDLE";
        case TEL_DIALTONE: return "DIALTONE";
        case TEL_DIALING: return "DIALING";
        case TEL_RINGBACK: return "RINGBACK";
        case TEL_RINGING: return "RINGING";
        case TEL_INCALL: return "INCALL";
        case TEL_BUSY: return "BUSY";
        case TEL_REORDER: return "REORDER";
        case TEL_PLAYING: return "PLAYING";
        case TEL_RECORDING: return "RECORDING";
        default: return "?";
    }
}

tel_state_t telephony_state(int line)
{
    return (line >= 0 && line < NLINES) ? s_line[line].state : TEL_IDLE;
}
const char *telephony_last_number(int line)
{
    return (line >= 0 && line < NLINES) ? s_line[line].last_number : "";
}

const char *telephony_caller_name(int line)
{
    return (line >= 0 && line < NLINES) ? s_line[line].caller_name : "";
}

const char *telephony_caller_number(int line)
{
    return (line >= 0 && line < NLINES) ? s_line[line].caller_number : "";
}

char telephony_last_dtmf(int line, int64_t *at_us)
{
    if (line < 0 || line >= NLINES) return 0;
    if (at_us) *at_us = s_line[line].last_dtmf_us;
    return s_line[line].last_dtmf_digit;
}

/* ---- callbacks (run in other task contexts; just enqueue) ---- */
static void ag_cb(const ag1170_event_t *ev, void *ctx)
{
    in_evt_t e = { .line = ev->line, .digit = 0 };
    switch (ev->type) {
        case AG1170_EV_OFFHOOK: e.type = IN_OFFHOOK; break;
        case AG1170_EV_ONHOOK:  e.type = IN_ONHOOK;  break;
        case AG1170_EV_DIGIT:   e.type = IN_DIGIT; e.digit = (char)('0' + ev->digit); break;
        default: return;
    }
    xQueueSend(s_q, &e, 0);
}

static void dtmf_tap(int line, const int16_t *mono, size_t n, void *ctx)
{
    char d = dtmf_detector_push(&s_line[line].det, mono, n);
    if (d) {
        /* Recorded for tonemon/bringup introspection regardless of key --
         * the detector runs continuously on every audio block, independent
         * of dial state, so this is a free non-invasive tap point. */
        s_line[line].last_dtmf_digit = d;
        s_line[line].last_dtmf_us = esp_timer_get_time();
    }
    if (d >= '0' && d <= '9') {
        in_evt_t e = { .line = line, .type = IN_DIGIT, .digit = d };
        xQueueSend(s_q, &e, 0);
    }
}

/* player_play()'s done_cb -- runs on the player task, just enqueues like
 * every other input source (ag_cb/dtmf_tap/voip_cb) so the FSM only ever
 * changes state from its own task. */
static void clip_done_cb(int line, void *ctx)
{
    in_evt_t e = { .line = line, .type = IN_CLIP_DONE, .digit = 0 };
    xQueueSend(s_q, &e, 0);
}

/* recorder_start()'s done_cb -- same pattern: enqueue so the FSM decides what
 * happens next (back to dial tone, or idle if the caller already hung up). */
static void rec_done_cb(int line, bool ok, void *ctx)
{
    in_evt_t e = { .line = line, .type = IN_REC_DONE, .digit = 0 };
    xQueueSend(s_q, &e, 0);
}

static void voip_cb(const voip_event_t *ev, void *ctx)
{
    in_evt_t e = { .line = ev->line, .digit = 0 };
    switch (ev->type) {
        case VOIP_EV_INCOMING: e.type = IN_VOIP_INCOMING; break;
        case VOIP_EV_ANSWERED: e.type = IN_VOIP_ANSWERED; break;
        case VOIP_EV_CLOSED:   e.type = IN_VOIP_CLOSED;   break;
        default: return; /* registration events: ignore for FSM */
    }
    xQueueSend(s_q, &e, 0);
}

/* ---- state helpers ---- */
static void enter_idle(int line)
{
    ESP_LOGD(TAG, "line %d: %s -> IDLE", line, telephony_state_name(s_line[line].state));
    audio_tx_silence(line);
    s_line[line].state = TEL_IDLE;
    s_line[line].peer = PEER_NONE;
    s_line[line].dialing = false;
    s_line[line].tone_return_us = 0;
    dialplan_collector_reset(&s_line[line].coll);
}

static void enter_dialtone(int line)
{
    ESP_LOGD(TAG, "line %d: %s -> DIALTONE", line, telephony_state_name(s_line[line].state));
    audio_tx_tone(line, TONE_DIAL);
    s_line[line].state = TEL_DIALTONE;
    s_line[line].peer = PEER_NONE;
    s_line[line].dialing = false;
    s_line[line].tone_return_us = 0;
    dialplan_collector_reset(&s_line[line].coll);
}

void telephony_force_recover(int line)
{
    if (line < 0 || line >= NLINES) return;
    s_line[line].tone_return_us = 0;
    if (line_offhook(line)) {
        enter_dialtone(line);
    } else {
        enter_idle(line);
    }
    ESP_LOGI(TAG, "line %d: forced recovery to %s", line, telephony_state_name(s_line[line].state));
}

static volatile bool s_monitor_suppress;

void telephony_set_monitor_suppress(bool on)
{
    s_monitor_suppress = on;
    ESP_LOGI(TAG, "monitor suppress %s", on ? "ON (digits dropped before dial plan)" : "off");
}

static void play_transient(int line, call_progress_t tone, uint32_t ms,
                           tel_state_t state, tel_state_t return_state)
{
    ESP_LOGD(TAG, "line %d: %s -> %s (transient tone %u ms, then %s)", line,
             telephony_state_name(s_line[line].state), telephony_state_name(state),
             (unsigned)ms, telephony_state_name(return_state));
    audio_tx_tone(line, tone);
    s_line[line].state = state;
    s_line[line].tone_return_us = esp_timer_get_time() + (int64_t)ms * 1000;
    s_line[line].tone_return_state = return_state;
}

/* persistent tone until hangup (e.g. busy / call-ended reorder) */
static void play_persistent(int line, call_progress_t tone, tel_state_t state)
{
    ESP_LOGD(TAG, "line %d: %s -> %s (persistent tone until hangup)", line,
             telephony_state_name(s_line[line].state), telephony_state_name(state));
    audio_tx_tone(line, tone);
    s_line[line].state = state;
    s_line[line].tone_return_us = 0;
}

/* ---- inter-line local call ---- */
static void start_local_call(int caller, int target, voice_effect_t fx)
{
    if (target < 0 || target >= NLINES || target == caller) {
        play_transient(caller, TONE_REORDER, 3000, TEL_REORDER, TEL_DIALTONE);
        return;
    }
    if (s_line[target].state != TEL_IDLE || line_offhook(target)) {
        play_persistent(caller, TONE_BUSY, TEL_BUSY);
        return;
    }
    /* ring the target, give caller ringback */
    s_line[target].state = TEL_RINGING;
    s_line[target].peer  = caller;
    ag1170_ring_start(target);

    s_line[caller].state = TEL_RINGBACK;
    s_line[caller].peer  = target;
    s_line[caller].call_fx = fx;
    audio_tx_tone(caller, TONE_RINGBACK);
    ESP_LOGI(TAG, "line %d calling line %d (fx=%d)", caller, target, fx);
}

static void connect_bridge(int a, int b)
{
    audio_tx_bridge(a, b, true);
    audio_tx_bridge(b, a, true);
    s_line[a].state = TEL_INCALL;
    s_line[b].state = TEL_INCALL;
    s_line[a].peer = b;
    s_line[b].peer = a;
    ESP_LOGI(TAG, "bridge %d <-> %d", a, b);
}

/* tear down whatever `line` is doing; notify peer appropriately */
static void end_call(int line)
{
    int peer = s_line[line].peer;

    audio_tx_bridge(line, peer, false);
    audio_tx_silence(line);
    voip_hangup(line);
    if (s_line[line].state == TEL_PLAYING) player_stop(line);
    if (s_line[line].state == TEL_RECORDING) recorder_stop(line);

    if (peer >= 0 && peer < NLINES) {
        switch (s_line[peer].state) {
            case TEL_RINGING:     /* callee never answered */
                ag1170_ring_stop(peer);
                enter_idle(peer);
                break;
            case TEL_INCALL:      /* connected -> tell peer call ended */
                audio_tx_bridge(peer, line, false);
                if (line_offhook(peer)) {
                    play_persistent(peer, TONE_REORDER, TEL_REORDER);
                    s_line[peer].peer = PEER_NONE;
                } else {
                    enter_idle(peer);
                }
                break;
            case TEL_RINGBACK:    /* caller waiting on us */
                play_persistent(peer, TONE_REORDER, TEL_REORDER);
                s_line[peer].peer = PEER_NONE;
                break;
            default:
                break;
        }
    }
    enter_idle(line);
}

/* ---- dial action dispatch ---- */
static void handle_action(int line, const dialplan_action_t *a)
{
    switch (a->kind) {
        case ACT_NONE:
            return;

        case ACT_SET_MODE:
            dialplan_apply_mode(line, (dialplan_mode_change_t)a->arg);
            play_transient(line, TONE_CONFIRM, 700, TEL_DIALTONE, TEL_DIALTONE);
            break;

        case ACT_OPERATOR_HELP:
            if (player_play(line, "operator.wav", clip_done_cb, NULL) == ESP_OK) {
                s_line[line].state = TEL_PLAYING;
            } else {
                ESP_LOGI(TAG, "line %d: operator help (no operator.wav in storage yet)", line);
                play_transient(line, TONE_CONFIRM, 700, TEL_DIALTONE, TEL_DIALTONE);
            }
            break;

        case ACT_PLAY_CLIP: /* dial-code entry (Settings -> Dial Codes), a->filename set directly */
            if (player_play(line, a->filename, clip_done_cb, NULL) == ESP_OK) {
                s_line[line].state = TEL_PLAYING;
            } else {
                ESP_LOGI(TAG, "line %d: play '%s' (not in storage?)", line, a->filename);
                play_transient(line, TONE_CONFIRM, 700, TEL_DIALTONE, TEL_DIALTONE);
            }
            break;

        case ACT_RECORD_ARCHIVE: { /* 211: record a message to storage */
            rec_name_timestamped(s_line[line].rec_name, sizeof(s_line[line].rec_name), line);
            audio_tx_silence(line);
            if (recorder_start(line, s_line[line].rec_name, 30000, 2000,
                               rec_done_cb, NULL) == ESP_OK) {
                s_line[line].state = TEL_RECORDING;
            } else {
                ESP_LOGW(TAG, "line %d: record-to-archive couldn't start", line);
                play_transient(line, TONE_CONFIRM, 700, TEL_DIALTONE, TEL_DIALTONE);
            }
            break;
        }

        case ACT_RADIO:
            ESP_LOGI(TAG, "line %d: radio station %d (stream TODO)", line, a->arg);
            play_transient(line, TONE_CONFIRM, 700, TEL_DIALTONE, TEL_DIALTONE);
            break;

        case ACT_CALL_LINE:
            start_local_call(line, a->arg, (voice_effect_t)a->arg2);
            break;

        case ACT_VOIP_DIAL: {
            strncpy(s_line[line].last_number, a->number, sizeof(s_line[line].last_number) - 1);
            esp_err_t r = voip_dial(line, a->number);
            if (r == ESP_OK) {
                audio_tx_tone(line, TONE_RINGBACK);
                s_line[line].state = TEL_RINGBACK;
                s_line[line].peer = PEER_VOIP;
            } else {
                ESP_LOGW(TAG, "line %d: VoIP dial %s -> %s (reorder)",
                         line, a->number, esp_err_to_name(r));
                play_persistent(line, TONE_REORDER, TEL_REORDER);
            }
            break;
        }

        case ACT_WRONG_NUMBER:
            ESP_LOGI(TAG, "line %d: wrong number '%s'", line, s_line[line].coll.buf);
            play_transient(line, TONE_REORDER, 3000, TEL_REORDER, TEL_DIALTONE);
            break;
    }
    dialplan_collector_reset(&s_line[line].coll);
    s_line[line].dialing = false;
}

static void process_digit(int line, char ch)
{
    if (s_monitor_suppress) return; /* pulsemon/tonemon active: don't feed the dial plan */

    tel_state_t st = s_line[line].state;
    if (st != TEL_DIALTONE && st != TEL_DIALING) {
        return; /* ignore digits unless we're expecting them */
    }
    if (st == TEL_DIALTONE) {
        audio_tx_silence(line); /* stop dial tone on first digit */
    }
    s_line[line].state = TEL_DIALING;
    s_line[line].dialing = true;
    s_line[line].last_digit_us = esp_timer_get_time();

    /* optional digit readback: short DTMF feedback burst (ESP adaptation of the
     * Pi's spoken-digit readback; auto-returns to silence, collection unaffected) */
    if (dialplan_modes()->readback_digits) {
        audio_tx_dtmf(line, ch, 120);
    }

    dialplan_action_t a = dialplan_feed(&s_line[line].coll, line, ch);
    ESP_LOGD(TAG, "line %d: digit '%c' -> collected '%s', action kind=%d", line,
             ch, s_line[line].coll.buf, a.kind);
    if (a.kind != ACT_NONE) {
        handle_action(line, &a);
    }
}

/* ---- main event handlers ---- */
static void on_offhook(int line)
{
    tel_state_t st = s_line[line].state;
    if (st == TEL_RINGING) {
        /* answering an incoming call */
        int peer = s_line[line].peer;
        if (peer == PEER_VOIP) {
            if (voip_answer(line) == ESP_OK) {
                audio_tx_external(line, true);
                s_line[line].state = TEL_INCALL;
            } else {
                play_persistent(line, TONE_REORDER, TEL_REORDER);
            }
        } else if (peer >= 0 && peer < NLINES) {
            connect_bridge(line, peer);
        } else {
            enter_dialtone(line);
        }
        return;
    }
    /* normal pickup */
    enter_dialtone(line);
}

static void on_onhook(int line)
{
    /* Hanging up ends a recording: stop it and let IN_REC_DONE finalize
     * (archive keeps what was captured; a callback is abandoned since the
     * caller is gone). Don't enter_idle here -- the done handler does. */
    if (s_line[line].state == TEL_RECORDING) {
        recorder_stop(line);
        return;
    }
    end_call(line);
}

static void on_voip_incoming(int line)
{
    if (s_line[line].state != TEL_IDLE) {
        voip_hangup(line); /* busy */
        return;
    }
    /* Snapshot caller ID now: voip clears it when the call ends, but the
     * web UI wants to still show who last called after that. */
    strlcpy(s_line[line].caller_name, voip_peer_name(line), sizeof(s_line[line].caller_name));
    strlcpy(s_line[line].caller_number, voip_peer_number(line), sizeof(s_line[line].caller_number));
    ESP_LOGI(TAG, "line %d: ringing -- caller %s <%s>", line,
             s_line[line].caller_name[0] ? s_line[line].caller_name : "(no name)",
             s_line[line].caller_number);
    s_line[line].state = TEL_RINGING;
    s_line[line].peer = PEER_VOIP;
    ag1170_ring_start(line);
}

static void fsm_task(void *arg)
{
    const TickType_t tick = pdMS_TO_TICKS(10);
    while (1) {
        in_evt_t e;
        while (xQueueReceive(s_q, &e, 0) == pdTRUE) {
            /* Drop events for a line with no hardware (phantom line on a
             * board configured for fewer lines than the array width). */
            if (e.line < 0 || e.line >= NLINES_ACTIVE) continue;
            switch (e.type) {
                case IN_OFFHOOK:        on_offhook(e.line); break;
                case IN_ONHOOK:         on_onhook(e.line);  break;
                case IN_DIGIT:          process_digit(e.line, e.digit); break;
                case IN_VOIP_INCOMING:  on_voip_incoming(e.line); break;
                case IN_VOIP_ANSWERED:
                    if (s_line[e.line].state == TEL_RINGBACK) {
                        audio_tx_external(e.line, true);
                        s_line[e.line].state = TEL_INCALL;
                    }
                    break;
                case IN_VOIP_CLOSED:
                    /* Far end hung up (or declined/never answered). Give
                     * dial tone if still off-hook, ready to dial again
                     * immediately, rather than a persistent reorder tone
                     * that forces an on-hook/off-hook cycle first -- this
                     * is a private extension line, not a PSTN trunk, so
                     * there's no "your call cannot be completed" signal
                     * worth holding up the line for here. */
                    if (s_line[e.line].peer == PEER_VOIP) {
                        if (line_offhook(e.line))
                            enter_dialtone(e.line);
                        else
                            enter_idle(e.line);
                    }
                    break;
                case IN_CLIP_DONE:
                    /* Ignore if we've already moved on (e.g. hung up mid-clip --
                     * end_call() already handled state via player_stop()). */
                    if (s_line[e.line].state == TEL_PLAYING) {
                        if (line_offhook(e.line)) enter_dialtone(e.line);
                        else enter_idle(e.line);
                    }
                    break;
                case IN_REC_DONE: {
                    int l = e.line;
                    if (s_line[l].state != TEL_RECORDING) break; /* already moved on */
                    if (line_offhook(l)) enter_dialtone(l);
                    else enter_idle(l);
                    break;
                }
            }
        }

        int64_t now = esp_timer_get_time();
        for (int line = 0; line < NLINES_ACTIVE; line++) {
            /* inter-digit timeout */
            if (s_line[line].dialing &&
                (now - s_line[line].last_digit_us) / 1000 >= dialplan_interdigit_ms()) {
                dialplan_action_t a = dialplan_timeout(&s_line[line].coll, line);
                s_line[line].dialing = false;
                if (a.kind != ACT_NONE) handle_action(line, &a);
            }
            /* transient tone auto-return */
            if (s_line[line].tone_return_us &&
                now >= s_line[line].tone_return_us) {
                s_line[line].tone_return_us = 0;
                tel_state_t ret = s_line[line].tone_return_state;
                ESP_LOGI(TAG, "line %d: transient tone done, returning to %s",
                         line, telephony_state_name(ret));
                if (ret == TEL_DIALTONE) enter_dialtone(line);
                else if (ret == TEL_IDLE) enter_idle(line);
            }
        }
        vTaskDelay(tick);
    }
}

esp_err_t telephony_start(void)
{
    s_q = xQueueCreate(32, sizeof(in_evt_t));
    if (!s_q) return ESP_ERR_NO_MEM;

    dialplan_init();

    for (int line = 0; line < NLINES; line++) {
        memset(&s_line[line], 0, sizeof(s_line[line]));
        s_line[line].state = TEL_IDLE;
        s_line[line].peer = PEER_NONE;
    }
    /* DTMF detector + RX tap only for lines that actually exist. */
    for (int line = 0; line < NLINES_ACTIVE; line++) {
        dtmf_detector_init(&s_line[line].det, audio_sample_rate());
        audio_set_rx_tap(line, dtmf_tap, NULL);
    }

    ag1170_set_event_cb(ag_cb, NULL);
    voip_init(voip_cb, NULL);

    BaseType_t ok = xTaskCreate(fsm_task, "tel_fsm", 4096, NULL, 5, NULL);
    if (ok != pdPASS) return ESP_FAIL;

    ESP_LOGI(TAG, "telephony started (%d line%s serviced)", NLINES_ACTIVE,
             NLINES_ACTIVE == 1 ? "" : "s");
    return ESP_OK;
}

/* ---- test/verification injection (host DVT harness) ---- */
void telephony_test_set_enabled(bool on)
{
    s_test_enabled = on;
    if (!on) {
        for (int i = 0; i < NLINES; i++) s_test_offhook[i] = false;
    }
    ESP_LOGW(TAG, "TEST MODE %s%s", on ? "ENABLED" : "disabled",
             on ? " (hook/digit injection active)" : "");
}

bool telephony_test_enabled(void)
{
    return s_test_enabled;
}

esp_err_t telephony_test_hook(int line, bool offhook)
{
    if (!s_test_enabled) return ESP_ERR_INVALID_STATE;
    if (line < 0 || line >= NLINES_ACTIVE) return ESP_ERR_INVALID_ARG;
    s_test_offhook[line] = offhook;          /* shadow the SLIC before the FSM sees it */
    in_evt_t e = { .line = line, .type = offhook ? IN_OFFHOOK : IN_ONHOOK, .digit = 0 };
    return (xQueueSend(s_q, &e, pdMS_TO_TICKS(50)) == pdTRUE) ? ESP_OK : ESP_FAIL;
}

esp_err_t telephony_test_digit(int line, char digit)
{
    if (!s_test_enabled) return ESP_ERR_INVALID_STATE;
    if (line < 0 || line >= NLINES_ACTIVE) return ESP_ERR_INVALID_ARG;
    if (digit < '0' || digit > '9') return ESP_ERR_INVALID_ARG;
    in_evt_t e = { .line = line, .type = IN_DIGIT, .digit = digit };
    return (xQueueSend(s_q, &e, pdMS_TO_TICKS(50)) == pdTRUE) ? ESP_OK : ESP_FAIL;
}
