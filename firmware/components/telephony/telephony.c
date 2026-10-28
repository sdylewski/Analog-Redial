#include "telephony.h"
#include "dialplan.h"
#include "ag1170.h"
#include "board_pins.h"   /* BOARD_NUM_LINES */
#include "audio.h"
#include "dtmf.h"
#include "tones.h"
#include "voip.h"
#include "homecall.h"
#include "player.h"
#include "recorder.h"
#include "storage.h"
#include "activity.h"
#include "webhook.h"
#include "callcap.h"
#include "bt_companion.h"
#include "auxpins.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "telephony";

/* SNTP (see wifi.c) sets the system clock without a TZ, so this is UTC.
 * Before it syncs, time() reads from the 1970 epoch -- anything before
 * ~2024 is obviously unsynced, so fall back to a boot-relative tag rather
 * than naming every early recording "19700101_000003.wav" and colliding.
 *
 * No line number in the name (dropped 2026-09-14 -- the old "rec_l<N>_..."
 * form reads as the digits "10" at a glance, not "line 0", and confused
 * more than it disambiguated). Two lines finishing a 211 in the exact same
 * second would collide and one recording would overwrite the other, but
 * that's a same-second coincidence across two separate handsets, not a
 * real-world scenario worth the readability cost of keeping the line
 * number in every filename. */
static void rec_name_timestamped(char *out, size_t out_len)
{
    time_t now = time(NULL);
    if (now < 1700000000) {
        snprintf(out, out_len, "rec_boot%lld.wav",
                 (long long)(esp_timer_get_time() / 1000));
        return;
    }
    struct tm t;
    localtime_r(&now, &t);   /* local wall clock -- see the note on naming below */
    snprintf(out, out_len, "rec_%04d%02d%02d_%02d%02d%02d.wav",
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
#define PEER_BT       (-3) /* the Bluetooth-paired phone's call, via bt_companion */

/* What an in-progress TEL_PLAYING_RECORD_PROMPT/TEL_RECORD_BEEP/TEL_RECORDING
 * sequence is actually for -- all three purposes share the exact same state
 * shape (play something, beep, record), but differ in which file plays,
 * which audio source is captured, what filename the result gets, and how
 * finishing is handled. See start_recording_archive/_greeting/_voicemail()
 * and the IN_CLIP_DONE/tone_return_us/IN_REC_DONE dispatches below. */
typedef enum {
    REC_NONE = 0,
    REC_ARCHIVE,   /* 211: leave a message, self-recorded */
    REC_GREETING,  /* 212: record this line's answering-machine greeting */
    REC_VOICEMAIL, /* answering machine capturing a caller's message */
} rec_purpose_t;

/* internal event queue (decouples ISR/other-task callbacks from the FSM) */
typedef enum {
    IN_OFFHOOK = 0, IN_ONHOOK, IN_DIGIT,
    IN_VOIP_INCOMING, IN_VOIP_ANSWERED, IN_VOIP_CLOSED,
    IN_CLIP_DONE,
    IN_REC_DONE,
    IN_BT_CALL,   /* .digit carries the new bt_call_state_t */
} in_type_t;

typedef struct {
    int       line;
    in_type_t type;
    char      digit;
    uint8_t   kind;     /* IN_REC_DONE: the rec_purpose_t it was started for */
} in_evt_t;

typedef struct {
    tel_state_t          state;
    dialplan_collector_t coll;
    bool                 bt_music_pass; /* in BT_PASSTHROUGH only because the paired phone is playing music */
    int64_t              bt_music_seen_us; /* last time that music was seen playing */
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
    bool                 dialout_tone;   /* the second dial tone (after the dial-out 9) is playing */
    tel_state_t          tone_return_state;
    /* off-hook alert: when the current dial-tone dwell expires (0 = not
     * armed, i.e. not sitting on dial tone or the feature is disabled) */
    int64_t              offhook_alert_at_us;
    /* intercom: when dial tone has gone unused long enough to ring the other line (0 = not armed) */
    int64_t              intercom_at_us;
    /* Reading a string of digits aloud (dial 314, pi): the string, how far
     * through it we are, when to start the next clip, and whether one is
     * playing right now. NULL = not reading. */
    const char          *seq;
    uint16_t             seq_i;
    int64_t              seq_next_us;
    bool                 seq_playing;
    /* Timer (dial 846). timer_digits/timer_n: the minutes being entered;
     * timer_wait_until_us: give up waiting for a first digit at this time.
     * timer_end_us: when the running timer goes off (0 = none); it rings
     * timer_rounds more times, timer_next_us apart. Kept in RAM only. */
    char                 timer_digits[5];
    uint8_t              timer_n;
    int64_t              timer_wait_until_us;
    int64_t              timer_end_us;
    int64_t              timer_next_us;
    uint8_t              timer_rounds;
    bool                 timer_ringing;    /* timer_end_us has passed and the bell rounds are running */
    bool                 timer_announce;   /* play TIMER_DONE_NAME once the line is on dial tone */
    int                  timer_minutes;    /* minutes accepted, counting down from the hang-up */
    bool                 timer_accepted;   /* the minutes are in; waiting to hang up on the caller */
    int64_t              timer_hangup_us;  /* when the line hangs up on the caller */
    /* Alarm (dial 256): entry reuses the timer's digit collection (entry_alarm
     * says which one is being asked). alarm_at is when it rings (wall clock,
     * kept in flash); it then rings alarm_rounds more times. */
    bool                 entry_alarm;
    time_t               alarm_at;
    bool                 alarm_ringing;
    uint8_t              alarm_rounds;
    int64_t              alarm_next_us;
    bool                 alarm_announce;   /* play ALARM_DONE_NAME once the line is on dial tone */
    /* A spoken readback digit was started (it ends by itself and then
     * silences the line, so anything that sets new audio stops it first). */
    bool                 rb_clip;
    /* active recording (211 archive, 212 greeting, or answering-machine
     * voicemail -- see rec_purpose_t) */
    char                 rec_name[STORAGE_MAX_NAME];
    rec_purpose_t        rec_purpose;
    /* Set right before recorder_stop()/player_stop() when a real off-hook
     * on the OTHER (called) line interrupts an in-progress local-peer
     * voicemail -- see on_offhook()'s barge-in handling. Tells IN_REC_DONE
     * to connect the two live instead of the normal REC_VOICEMAIL cleanup. */
    bool                 am_bargein;
    /* esp_timer_get_time() when ag1170_ring_start() was last called on this
     * line (0 = not currently ringing/not tracked) -- the answering
     * machine's ring counter derives "how many rings so far" from this
     * rather than a separate per-cycle counter; see the FSM tick loop. */
    int64_t              ring_started_us;
    /* board_am_get(line) snapshotted at the same moment as ring_started_us,
     * NOT re-read every tick -- the ring-count check runs every 10ms while
     * ringing, and board_am_get() hits NVS each call; re-reading it that
     * often would be ~100 NVS opens/sec per ringing line for no benefit
     * (also means settings changed mid-ring don't retroactively change an
     * already-ringing call's behavior, which is the more sensible
     * semantic anyway). */
    board_am_cfg_t        ring_am_cfg;
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
        case TEL_OFFHOOK_ALERT: return "OFFHOOK_ALERT";
        case TEL_PLAYING_RECORD_PROMPT: return "PLAYING_RECORD_PROMPT";
        case TEL_RECORD_BEEP: return "RECORD_BEEP";
        case TEL_TIMER_ENTRY: return "TIMER_ENTRY";
        case TEL_BT_PASSTHROUGH: return "BT_PASSTHROUGH";
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
    /* On-hook, the only thing on this input is SLIC/line noise -- which the
     * Goertzel detector was intermittently reading as a real '6' continuously
     * while idle (seen live: a steady stream of false START/END pairs with
     * nobody near the phone). Gating on off-hook matches every real source of
     * DTMF (a live call, or `tonemon`'s own instructions to dial while off-
     * hook) and costs nothing: bench-testing generated tones via test_loopback
     * (audio.c) still goes through this same tap when actually off-hook, and
     * `dtmf_gen`'s own test script (test_dtmf.py) never touches the detector
     * at all, just plays tones for a human to listen to. */
    if (!line_offhook(line)) return;
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

static void note_new_message(void);   /* message waiting, below */

/* recorder_start()'s done_cb -- same pattern: enqueue so the FSM decides what
 * happens next (back to dial tone, or idle if the caller already hung up). */
static void rec_done_cb(int line, bool ok, void *ctx)
{
    /* ctx carries the purpose the recording was started for: end_call()
     * clears rec_purpose before this event arrives (a caller hanging up on
     * the answering machine), so it can't be read back from the line. */
    in_evt_t e = { .line = line, .type = IN_REC_DONE, .digit = ok ? 1 : 0,
                   .kind = (uint8_t)(intptr_t)ctx };
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

/* Runs in bt_companion's task -- just queue it, like every other source. */
static void bt_call_cb(bt_call_state_t state, void *ctx)
{
    (void)ctx;
    in_evt_t e = { .line = 0, .type = IN_BT_CALL, .digit = (char)state };
    xQueueSend(s_q, &e, 0);
}

/* ---- state helpers ---- */
static void enter_idle(int line)
{
    ESP_LOGD(TAG, "line %d: %s -> IDLE", line, telephony_state_name(s_line[line].state));
    /* Every hang-up path ends here: never carry an effect into the next call. */
    if (audio_voice_fx(line) != VOICE_FX_NONE) {
        ESP_LOGI(TAG, "line %d: clearing voice effect %s", line, voice_fx_name(audio_voice_fx(line)));
        audio_set_voice_fx(line, VOICE_FX_NONE);
    }
    s_line[line].call_fx = FX_NORMAL;
    s_line[line].bt_music_pass = false;
    s_line[line].seq = NULL;
    s_line[line].timer_n = 0;
    s_line[line].timer_announce = false;
    s_line[line].timer_accepted = false;
    s_line[line].entry_alarm = false;
    s_line[line].alarm_announce = false;
    if (s_line[line].rb_clip) { player_stop(line); s_line[line].rb_clip = false; }
    audio_tx_silence(line);
    s_line[line].state = TEL_IDLE;
    s_line[line].peer = PEER_NONE;
    s_line[line].dialing = false;
    s_line[line].dialout_tone = false;
    s_line[line].tone_return_us = 0;
    dialplan_collector_reset(&s_line[line].coll);
    /* Deliberately does NOT touch AEC's enabled state. It's only ever
     * turned on by hand via the `aec`/`aec sweep` bring-up commands, and
     * stays on across hangup on purpose -- `aec sweep` needs a fresh call
     * to re-measure against, and forcing it off here would erase that
     * mid-investigation. (A prior version of this function auto-disabled
     * AEC here as a mitigation for a watchdog-panic hang seen right after
     * `aec sweep` + hangup; testing showed it didn't actually prevent that
     * hang -- the runaway starts before this function gets a chance to run
     * -- so it was only costing the "stays on for the next call" behavior
     * without buying real protection. `aec sweep` prints its own reminder
     * to run `aec off <line>` when done instead.) */
}

static void enter_dialtone(int line)
{
    /* A readback digit finishing now would silence the dial tone below. */
    if (s_line[line].rb_clip) { player_stop_and_wait(line, 150); s_line[line].rb_clip = false; }
    ESP_LOGD(TAG, "line %d: %s -> DIALTONE", line, telephony_state_name(s_line[line].state));
    audio_tx_tone(line, TONE_DIAL);
    s_line[line].state = TEL_DIALTONE;
    s_line[line].peer = PEER_NONE;
    s_line[line].dialing = false;
    s_line[line].dialout_tone = false;
    s_line[line].tone_return_us = 0;
    dialplan_collector_reset(&s_line[line].coll);

    /* (Re)start the off-hook dwell clock. Read per entry rather than cached
     * at boot so a settings change applies to the next dial tone without a
     * reboot. Every path back to dial tone (hang-up of a call, inter-digit
     * timeout, clip finishing) comes through here, so the timer restarts
     * whenever the user is plausibly "back at the top" again -- only a
     * genuinely untouched handset accumulates the full dwell. */
    board_offhook_alert_t roh = board_offhook_alert_get();
    s_line[line].offhook_alert_at_us =
        roh.enabled ? esp_timer_get_time() + (int64_t)roh.seconds * 1000000 : 0;

    /* Intercom: same idea, and likewise self-invalidating -- only consulted
     * while still in DIALTONE, so dialling a digit cancels it. */
    board_intercom_t ic = board_intercom_get();
    s_line[line].intercom_at_us =
        (ic.enabled && NLINES_ACTIVE > 1) ? esp_timer_get_time() + (int64_t)ic.seconds * 1000000 : 0;
}

/* Which line the Bluetooth companion claims. Always line 0: the companion's
 * analog audio is wired to line 0's Vin/Vout on every board that has it --
 * SMD4/newphone by bench wiring, and SMD5 by its J12 header, whose pin table
 * names those nets Line0Vin/Line0Vout outright.
 *
 * History worth keeping, because this flipped twice: it was line 0, moved to
 * line 1 (2026-09-17) to stop a stray bt_enabled=true in NVS from silently
 * killing dial tone on the one line every board has, then moved back here
 * once the hardware was pinned down -- routing passthrough to line 1 while
 * the audio is physically on line 0 is just broken, and on a 2-line SMD5 it
 * would have been silently broken. The original safety concern is now handled
 * properly instead of by relocation: board_hw_config_get().has_bt_module says
 * whether the hardware even exists, so an enabled flag alone can no longer
 * claim a line (see bt_companion_active()). */
static int bt_companion_line(void)
{
    return 0;
}

/* Whether lifting the handset on `line` right now should hand it to the
 * Bluetooth companion instead of normal telephony. Deliberately checks
 * bt_connected, not just bt_companion_active() (enabled + a module fitted):
 * a board can sit with Bluetooth switched on for days with no phone actually
 * paired, and until this check existed that alone was enough to make line 0
 * permanently silent -- confirmed live 2026-09-27, `tone` played perfectly
 * over the exact same physical path that produced nothing on a real off-hook,
 * because BT_PASSTHROUGH was intercepting it with no live connection to hand
 * the line to. Matches the design intent written up in
 * archive/BLUETOOTH_AUDIO_UPGRADE.md: normal telephony should keep working
 * whenever Bluetooth isn't actively in use, not just whenever it's off. */
static void enter_bt_passthrough(int line);

static bool bt_companion_should_intercept(int line)
{
    if (line != bt_companion_line() || !bt_companion_active()) return false;
    /* Only a fresh pickup from idle. An off-hook while this line is ringing is
     * someone answering that call -- VoIP, local, or the Bluetooth phone's own
     * (on_offhook() handles all three) -- and must never be diverted into
     * passthrough instead. Before this check existed, lifting the handset to
     * answer a ringing VoIP call on line 0 would do exactly that whenever a
     * phone happened to be paired. */
    if (s_line[line].state != TEL_IDLE) return false;
    /* And only while the paired phone has a call up (e.g. placed from the
     * phone itself, audio routed here) or is playing music, whose audio
     * arrives on this line's earpiece directly and must not be mixed with
     * dial tone. With a phone merely connected the
     * pickup gets normal dial tone, so local codes and 1 + number dial-out
     * work -- a connected-but-idle passthrough was just dead air. */
    return bt_companion_get_status().bt_connected &&
           (bt_companion_call_live() || bt_companion_music_playing());
}

/* Confirmed 2026-09-15 on real bench hardware (then still on line 0 -- see
 * this file's git history and the correction above): normal off-hook
 * handling (enter_dialtone(), and later the 15s inter-digit timeout's
 * wrong-number tone) plays audio through this line's own codec DAC -- which
 * shares the exact same physical Vin node the Bluetooth companion's analog
 * bridge also drives. Two active DAC outputs fighting the same node isn't a
 * "wrong tone", it's electrical contention: real testing showed it
 * corrupting the Bluetooth audio and being a plausible cause of the phone's
 * SCO link dropping outright a few seconds after the dial-collector timeout
 * fired. While the companion is enabled, this line skips dial tone/digit
 * collection/the timeout entirely and just stays silent on its own DAC --
 * the whole point of the line right now is being a dumb analog passthrough
 * for the companion, not a rotary-dial line a user actually dials from. */
static void enter_bt_passthrough(int line)
{
    ESP_LOGD(TAG, "line %d: %s -> BT_PASSTHROUGH", line, telephony_state_name(s_line[line].state));
    audio_tx_silence(line);
    s_line[line].bt_music_pass = false;
    s_line[line].state = TEL_BT_PASSTHROUGH;
    s_line[line].peer = PEER_NONE;
    s_line[line].dialing = false;
    s_line[line].dialout_tone = false;
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

/* 2min compiled fallback -- board_record_max_ms_get() returns whatever was
 * set via Settings -> Record message once it's been saved there. No warning
 * beeps before the cutoff (tried that, read as more alarming than helpful
 * for a generous limit like this) -- the recording just ends; the prompt
 * text tells the caller the limit up front instead. */
#define RECORD_MAX_MS_DEFAULT 120000

/* Answering machine's own greeting (212) -- short by nature (it's a greeting,
 * not a message), no per-board setting for it, unlike the caller's own
 * message length (board_am_get().max_ms, user-configurable). */
#define AM_GREETING_MAX_MS 60000

/* Trailing-silence auto-stop shared by every recording purpose below: even
 * 5000ms was still cutting real messages short on an ordinary pause mid-
 * message (confirmed live -- a caller got cut off around 15s into a 30s-max
 * recording, well before anything close to silent for that whole stretch).
 * 8000ms gives real room to think without sitting through the full max_ms on
 * every short message that just ends with a quiet hangup. */
#define RECORD_SILENCE_MS 8000

void telephony_am_greeting_name(char *out, size_t len, int line)
{
    snprintf(out, len, "am_greeting_l%d.wav", line);
}

/* Mirrors rec_name_timestamped() but with its own "voicemail_" prefix
 * (changed 2026-09-14 from a "rec_l<N>_am_" tag -- see storage.c's
 * is_protected_asset() and webui.c's is_user_recording(), both updated to
 * match "voicemail_" as an ordinary, evictable recording alongside "rec_",
 * same treatment as before, just its own readable prefix instead of a tag
 * buried after a line number). No line number, same reasoning as
 * rec_name_timestamped(). */
static void rec_name_voicemail(char *out, size_t out_len)
{
    time_t now = time(NULL);
    if (now < 1700000000) {
        snprintf(out, out_len, "voicemail_boot%lld.wav",
                 (long long)(esp_timer_get_time() / 1000));
        return;
    }
    struct tm t;
    localtime_r(&now, &t);   /* local wall clock -- see the note on naming below */
    snprintf(out, out_len, "voicemail_%04d%02d%02d_%02d%02d%02d.wav",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
}

/* 211: actually start capturing to storage. Factored out of
 * ACT_RECORD_ARCHIVE so the tick loop's tone-return path (after the beep)
 * can call it too, without duplicating the recorder_start()/failure logic. */
static void start_recording_archive(int line)
{
    rec_name_timestamped(s_line[line].rec_name, sizeof(s_line[line].rec_name));
    audio_tx_silence(line);
    uint32_t max_ms = board_record_max_ms_get(RECORD_MAX_MS_DEFAULT);
    if (recorder_start(line, s_line[line].rec_name, max_ms, RECORD_SILENCE_MS,
                       rec_done_cb, (void *)(intptr_t)REC_ARCHIVE) == ESP_OK) {
        s_line[line].state = TEL_RECORDING;
    } else {
        ESP_LOGW(TAG, "line %d: record-to-archive couldn't start", line);
        play_transient(line, TONE_CONFIRM, 700, TEL_DIALTONE, TEL_DIALTONE);
    }
}

/* 212: record this line's own answering-machine greeting. Fixed filename
 * (overwrites the previous greeting directly, like a real answering machine
 * -- reviewed/re-recorded via the web UI's playback, not a phone-side
 * confirm loop) -- see archive/ANSWERING_MACHINE.md. */
static void start_recording_greeting(int line)
{
    telephony_am_greeting_name(s_line[line].rec_name, sizeof(s_line[line].rec_name), line);
    audio_tx_silence(line);
    if (recorder_start(line, s_line[line].rec_name, AM_GREETING_MAX_MS, RECORD_SILENCE_MS,
                       rec_done_cb, (void *)(intptr_t)REC_GREETING) == ESP_OK) {
        s_line[line].state = TEL_RECORDING;
    } else {
        ESP_LOGW(TAG, "line %d: greeting recording couldn't start", line);
        play_transient(line, TONE_CONFIRM, 700, TEL_DIALTONE, TEL_DIALTONE);
    }
}

/* Beep cue before recording starts -- a single clean tone
 * (TONE_ANSWERING_BEEP), not TONE_CONFIRM's triple stutter (that read as a
 * generic "confirmation chime," not an answering-machine beep -- confirmed
 * live). Runs whether or not a prompt/greeting played first. Purpose-generic
 * -- which start_recording_*() the tick loop calls once this beep's timer
 * expires is driven by rec_purpose, set by the caller before this runs. */
#define RECORD_BEEP_MS 600
static void start_record_beep(int line)
{
    play_transient(line, TONE_ANSWERING_BEEP, RECORD_BEEP_MS, TEL_RECORD_BEEP, TEL_RECORDING);
}

/* Starts ringing `line` for an incoming call (local peer or VoIP) -- or, if
 * board_quiet_hours_active() (board.h) says it's currently quiet time,
 * skips the physical ring entirely and sets ring_am_cfg so the very next
 * FSM tick treats this exactly like a call that's already used up its ring
 * count (see the eligibility check in fsm_task() below), routing it
 * straight to the answering machine (auto_answer()) instead. Applies
 * regardless of whether Answering Machine is itself enabled/configured for
 * this line -- during quiet hours there's nowhere else to send an incoming
 * call, and auto_answer() already degrades gracefully with no greeting
 * recorded (straight to a beep, then record). Shared by start_local_call()
 * and on_voip_incoming() so this only lives in one place. */
static void start_ring(int line, bool intercom)
{
    s_line[line].ring_started_us = esp_timer_get_time();
    if (board_quiet_hours_active()) {
        board_am_cfg_t am = board_am_get(line);
        if (am.enabled) {
            /* Answering machine is set up: take the call silently, as if the
             * ring count had already run out. */
            s_line[line].ring_am_cfg = (board_am_cfg_t){
                .enabled = true, .rings = 0, .other_line = true, .max_ms = am.max_ms,
            };
        } else {
            /* No answering machine: stay silent and simply never answer. The
             * caller keeps hearing ringback until they give up, which reads
             * as "nobody's home" -- deliberately not a busy signal, which
             * would suggest someone is awake and on the phone. Forcing the
             * AM on here instead would quietly pile up messages the owner
             * never set up and may not know to check. */
            s_line[line].ring_am_cfg = (board_am_cfg_t){ .enabled = false };
        }
        return; /* no ag1170_ring_start() -- stay silent either way */
    }
    if (intercom) ag1170_ring_start_intercom(line);
    else          ag1170_ring_start(line);
    s_line[line].ring_am_cfg = board_am_get(line);
}

/* ---- inter-line local call ---- */
static void start_local_call_ex(int caller, int target, voice_effect_t fx, bool intercom)
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
    start_ring(target, intercom);

    s_line[caller].state = TEL_RINGBACK;
    s_line[caller].peer  = target;
    s_line[caller].call_fx = fx;
    audio_tx_tone(caller, TONE_RINGBACK);
    ESP_LOGI(TAG, "line %d calling line %d (fx=%d)", caller, target, fx);
    activity_log(intercom ? "Line %d rang line %d (intercom)" : "Line %d called line %d", caller, target);
    webhook_eventf("ring", target, "line %d", caller);
}

static void start_local_call(int caller, int target, voice_effect_t fx)
{
    start_local_call_ex(caller, target, fx, false);
}

/* Same effect for everyone in the call: both lines of a local bridge (each
 * shapes the other's voice), or this line alone for VoIP, where it covers both
 * directions -- the far end's voice in this earpiece, and this mic's uplink. */
/* One serial-monitor line of levels through `line`'s effect since the last
 * report (what its earpiece heard, then what the far end got). */
static void log_fx_stats(int line, const char *why)
{
    voice_fx_t fx = audio_voice_fx(line);
    if (fx == VOICE_FX_NONE) return;
    voice_fx_stats_t d, u;
    audio_voice_fx_stats(line, true, &d);
    audio_voice_fx_stats(line, false, &u);
    ESP_LOGI(TAG, "line %d fx %s (%s): earpiece in/out rms %u/%u peak %d/%d limited %u; "
             "uplink in/out rms %u/%u peak %d/%d limited %u",
             line, voice_fx_name(fx), why,
             (unsigned)d.in_rms, (unsigned)d.out_rms, (int)d.in_peak, (int)d.out_peak, (unsigned)d.limited,
             (unsigned)u.in_rms, (unsigned)u.out_rms, (int)u.in_peak, (int)u.out_peak, (unsigned)u.limited);
}

static void set_call_voice_fx(int line, voice_fx_t fx)
{
    int peer = s_line[line].peer;
    log_fx_stats(line, "switching");
    ESP_LOGI(TAG, "line %d: voice effect %s -> %s (peer %d)", line,
             voice_fx_name(audio_voice_fx(line)), voice_fx_name(fx), peer);
    if (audio_set_voice_fx(line, fx) != ESP_OK) {
        ESP_LOGW(TAG, "line %d: not enough memory for a voice effect", line);
        return;
    }
    if (peer >= 0 && peer < NLINES) audio_set_voice_fx(peer, fx);
    activity_log("Line %d: voice effect %s", line, voice_fx_name(fx));
}

static voice_fx_t voice_fx_for(voice_effect_t e)
{
    switch (e) {
    case FX_HIGH:  return VOICE_FX_CHIPMUNK;
    case FX_LOW:   return VOICE_FX_VADER;
    case FX_DALEK: return VOICE_FX_DALEK;
    default:       return VOICE_FX_NONE;
    }
}

/* A key pressed (or dialled, on a rotary phone) during a call: 1-7 pick an
 * effect, 0 turns it off. Set, not toggle: the tone also reaches the other
 * phone and can echo back into its own detector, and a second "set Dalek" is
 * harmless where a toggle would flip it straight back off. */
static void in_call_key(int line, char ch)
{
    static const voice_fx_t KEYS[] = {
        ['0' - '0'] = VOICE_FX_NONE,
        ['1' - '0'] = VOICE_FX_DALEK,
        ['2' - '0'] = VOICE_FX_CHIPMUNK,
        ['3' - '0'] = VOICE_FX_VADER,
        ['4' - '0'] = VOICE_FX_8BIT,
        ['5' - '0'] = VOICE_FX_ROBOT,
        ['6' - '0'] = VOICE_FX_TREMOLO,
        ['7' - '0'] = VOICE_FX_DISTORTION,
    };
    if (!board_voice_fx_enabled_get()) return;
    if (ch < '0' || ch > '7') return;
    voice_fx_t fx = KEYS[ch - '0'];
    ESP_LOGI(TAG, "line %d: in-call key '%c' -> %s", line, ch, voice_fx_name(fx));
    if (audio_voice_fx(line) == fx) return;
    set_call_voice_fx(line, fx);
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
    /* 322-324 picked an effect when dialling; whichever side dialled holds it. */
    voice_effect_t fx = s_line[a].call_fx != FX_NORMAL ? s_line[a].call_fx : s_line[b].call_fx;
    ESP_LOGI(TAG, "bridge %d <-> %d: dialled effect %d, effects %s", a, b, (int)fx,
             board_voice_fx_enabled_get() ? "enabled" : "disabled");
    if (fx != FX_NORMAL && board_voice_fx_enabled_get()) set_call_voice_fx(a, voice_fx_for(fx));
}

/* tear down whatever `line` is doing; notify peer appropriately */
static void end_call(int line)
{
    int peer = s_line[line].peer;

    s_line[line].seq = NULL;
    log_fx_stats(line, "call ended");
    if (peer >= 0 && peer < NLINES) log_fx_stats(peer, "call ended");
    audio_set_voice_fx(line, VOICE_FX_NONE);
    s_line[line].call_fx = FX_NORMAL;
    if (peer >= 0 && peer < NLINES) {
        audio_set_voice_fx(peer, VOICE_FX_NONE);
        s_line[peer].call_fx = FX_NORMAL;
    }

    /* Hanging up the handset during a Bluetooth call ends that call on the
     * phone, as with any headset. Only for a real call -- passthrough with
     * just music playing (A2DP, no HF call) is left alone. */
    if (s_line[line].state == TEL_BT_PASSTHROUGH && bt_companion_call_live()) {
        bt_companion_end_call();
    }

    /* Capture before anything below tears down state/peer. */
    if (peer >= 0 && peer < NLINES) {
        activity_log("Line %d: call with line %d ended", line, peer);
    } else if (peer == PEER_VOIP) {
        activity_log("Line %d: VoIP call ended", line);
    } else {
        activity_log("Line %d: call ended", line);
    }

    audio_tx_bridge(line, peer, false);
    audio_tx_silence(line);
    audio_set_uplink_inject(line, false); /* no-op unless the answering machine had it on -- see auto_answer() */
    voip_hangup(line);
    if (s_line[line].state == TEL_PLAYING ||
        s_line[line].state == TEL_TIMER_ENTRY ||
        s_line[line].state == TEL_PLAYING_RECORD_PROMPT) player_stop(line);
    if (s_line[line].state == TEL_RECORDING) recorder_stop(line);
    s_line[line].rec_purpose = REC_NONE;

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

/* Answering machine: capture a caller's message on `rec_line` -- their own
 * line for a local-peer call (their mic really does carry their voice), or
 * the auto-answered line itself for a VoIP call (their voice only exists as
 * decoded downlink audio there -- see recorder_start_downlink() and
 * audio_add_extwrite_tap()'s doc comment for why the recorder taps ahead of
 * this line's own (silenced) TX rather than behind it). `for_line` names
 * whose mailbox this is (the line being called), for the filename and the
 * max-length setting -- differs from rec_line only in the local-peer case.
 * On failure, end_call(rec_line) rather than a confirm tone: there's no
 * local human on rec_line to play one to in the VoIP case, and in the local
 * case it'd be a strange thing for the *caller* to hear after already
 * being told to leave a message. */
static void start_recording_voicemail(int rec_line, int for_line)
{
    rec_name_voicemail(s_line[rec_line].rec_name, sizeof(s_line[rec_line].rec_name));
    audio_tx_silence(rec_line);
    board_am_cfg_t cfg = board_am_get(for_line);
    bool downlink = (s_line[rec_line].peer == PEER_VOIP);
    esp_err_t err = downlink
        ? recorder_start_downlink(rec_line, s_line[rec_line].rec_name, cfg.max_ms,
                                  RECORD_SILENCE_MS, rec_done_cb, (void *)(intptr_t)REC_VOICEMAIL)
        : recorder_start(rec_line, s_line[rec_line].rec_name, cfg.max_ms,
                         RECORD_SILENCE_MS, rec_done_cb, (void *)(intptr_t)REC_VOICEMAIL);
    if (err == ESP_OK) {
        s_line[rec_line].state = TEL_RECORDING;
    } else {
        ESP_LOGW(TAG, "line %d: answering-machine recording couldn't start", rec_line);
        end_call(rec_line);
    }
}

/* Answering machine: no one answered within the configured ring count (see
 * the ring-counter in the FSM tick loop below). Always called with the line
 * that was actually ringing -- VoIP or local peer. */
static void auto_answer(int line)
{
    ag1170_ring_stop(line);
    int peer = s_line[line].peer;
    int rec_line;

    if (peer == PEER_VOIP) {
        if (voip_answer(line) != ESP_OK) {
            enter_idle(line);
            return;
        }
        /* Loop this line's own TX (whatever player_play()/start_record_beep()
         * below select -- SRC_EXTERNAL for the greeting, SRC_TONE for the
         * beep) back into its uplink -- normally audio_rx_read()/the RTP tx
         * task reads the real mic, which is just room silence with no one
         * physically holding the handset. inject_uplink mirrors txL/txR
         * regardless of which source is currently selected, so nothing
         * else needs to pick a source here. See audio_set_uplink_inject()'s
         * doc comment in audio.h. */
        audio_set_uplink_inject(line, true);
        rec_line = line;
    } else if (peer >= 0 && peer < NLINES) {
        /* The CALLER's own line does the playing/recording -- their mic
         * really does carry their voice, same as any other local
         * recording. `line` (the called line) deliberately stays in
         * TEL_RINGING with peer still set, bell already silenced above --
         * so a real pickup on it still hits on_offhook()'s normal
         * RINGING/local-peer branch, which knows to treat that as a
         * barge-in on `peer` (see am_active()) instead of a plain
         * connect_bridge(). */
        rec_line = peer;
    } else {
        enter_idle(line);
        return;
    }

    activity_log("Line %d: answering machine picked up (%s)", line,
                 peer == PEER_VOIP ? "VoIP" : "other line");
    webhook_event("answering_machine", line, peer == PEER_VOIP ? "VoIP" : "other line");
    s_line[rec_line].rec_purpose = REC_VOICEMAIL;
    s_line[rec_line].am_bargein = false;
    /* This line's own recording (dial 899) first, then the shared default
     * (text-to-speech if generated, else the one built into the firmware),
     * then just the beep -- same fallback 211 uses when RECORD_PROMPT_NAME
     * doesn't exist. */
    char greeting[STORAGE_MAX_NAME];
    telephony_am_greeting_name(greeting, sizeof(greeting), line);
    if (player_play(rec_line, greeting, clip_done_cb, NULL) == ESP_OK ||
        player_play(rec_line, AM_DEFAULT_GREETING_NAME, clip_done_cb, NULL) == ESP_OK) {
        s_line[rec_line].state = TEL_PLAYING_RECORD_PROMPT;
    } else {
        start_record_beep(rec_line);
    }
}

/* Plays `name`, or the same name with .wav/.mp3 swapped if that's what the
 * board actually has. The built-in 412 clip moved from dialup.mp3 to
 * dialup.wav: a board's saved dial codes keep the old name, and only a USB
 * flash rewrites the bundled audio (an over-the-air update doesn't), so
 * either name can be the one present. */
static esp_err_t play_clip_any_ext(int line, const char *name)
{
    esp_err_t err = player_play(line, name, clip_done_cb, NULL);
    size_t n = strlen(name);
    if (err != ESP_ERR_NOT_FOUND || n < 5 || n >= STORAGE_MAX_NAME) return err;
    char alt[STORAGE_MAX_NAME];
    memcpy(alt, name, n + 1);
    if (!strcasecmp(alt + n - 4, ".mp3"))      memcpy(alt + n - 4, ".wav", 4);
    else if (!strcasecmp(alt + n - 4, ".wav")) memcpy(alt + n - 4, ".mp3", 4);
    else return err;
    return player_play(line, alt, clip_done_cb, NULL);
}

/* ---- spoken digits ---- */

/* The clip for a digit ("5" -> 5.wav, "." -> point.wav). */
static void digit_clip_name(char ch, char *out, size_t len)
{
    if (ch == '.') snprintf(out, len, "point.wav");
    else           snprintf(out, len, "%c.wav", ch);
}

/* Digit readback: say the digit just dialled. A clip already playing (the
 * last digit) is cut short; with no digit clips on the board, or no memory to
 * play one, it's the touch-tone burst it used to be. */
static void readback_digit(int line, char ch)
{
    char name[16];
    digit_clip_name(ch, name, sizeof(name));
    player_stop_and_wait(line, 100);
    if (player_play(line, name, NULL, NULL) == ESP_OK) s_line[line].rb_clip = true;
    else audio_tx_dtmf(line, ch, 120);
}

/* Pi, 3.1415...: dial 314. The leading 3, "point", then 100 decimals. */
static const char PI_DIGITS[] =
    "3."
    "1415926535" "8979323846" "2643383279" "5028841971" "6939937510"
    "5820974944" "5923078164" "0628620899" "8628034825" "3421170679";
_Static_assert(sizeof(PI_DIGITS) - 1 == 102, "pi: 3. plus 100 decimals");

/* Pause after the clip just played: a breath between groups of 5 and 10. */
static int64_t seq_gap_us(const char *seq, size_t played)
{
    if (played <= 2) return 20000;                 /* "3", "point", then straight on */
    size_t decimals = played - 2;
    if (decimals % 10 == 0) return 500000;
    if (decimals % 5 == 0)  return 250000;
    (void)seq;
    return 20000;
}

/* Logs every dial code whose audio file isn't on the board (the web UI shows
 * the same thing next to each code). Codes are kept either way. */
void telephony_check_clip_files(void)
{
    /* ~2.6 KB: on the heap, not app_main's stack. */
    dialplan_clip_entry_t *clips = malloc(DIALPLAN_MAX_CLIP_ENTRIES * sizeof(*clips));
    if (!clips) return;
    size_t n = dialplan_list_clip_entries(clips, DIALPLAN_MAX_CLIP_ENTRIES);
    for (size_t i = 0; i < n; i++) {
        char alt[STORAGE_MAX_NAME];
        storage_reader_t r;
        size_t len = strlen(clips[i].filename);
        bool ok = storage_read_open(clips[i].filename, &r) == ESP_OK;
        if (!ok && len >= 5 && len < STORAGE_MAX_NAME) {
            memcpy(alt, clips[i].filename, len + 1);
            if (!strcasecmp(alt + len - 4, ".mp3"))      memcpy(alt + len - 4, ".wav", 4);
            else if (!strcasecmp(alt + len - 4, ".wav")) memcpy(alt + len - 4, ".mp3", 4);
            else alt[0] = '\0';
            ok = alt[0] && storage_read_open(alt, &r) == ESP_OK;
        }
        if (ok) storage_read_close(&r);
        else ESP_LOGW(TAG, "dial code %s: audio file %s is not on this board", clips[i].code, clips[i].filename);
    }
    free(clips);
}

/* ---- dial action dispatch ---- */
/* ---- timer (dial 846) ---------------------------------------------------- */
#define TIMER_FIRST_DIGIT_WAIT_US  (10LL * 1000000)  /* after the prompt, for the first digit */
#define TIMER_SECOND_DIGIT_WAIT_MS 3000              /* wait this long for a second digit (not the 15 s dial-plan timeout) */
#define TIMER_HANGUP_GRACE_MS      3000              /* after the minutes are accepted, before the line hangs up on the caller */
#define TIMER_RING_SECS            6
#define TIMER_RING_ROUNDS          4                 /* rings before it gives up */
#define TIMER_RING_EVERY_US        (20LL * 1000000)

/* The minutes are in. 0 cancels a running timer at once. Anything else is held
 * until the call ends: the countdown starts when the caller hangs up, or when
 * the line hangs up on them, `grace_ms` after the last digit (3 s, so a
 * mistake can still be heard before the line goes away). */
static void timer_accept(int line, int grace_ms)
{
    int minutes = atoi(s_line[line].timer_digits);
    s_line[line].timer_n = 0;
    s_line[line].timer_accepted = true;
    s_line[line].timer_minutes = minutes;
    s_line[line].timer_hangup_us = esp_timer_get_time() + (int64_t)grace_ms * 1000;
    if (minutes <= 0) {
        bool had = s_line[line].timer_end_us != 0;
        s_line[line].timer_end_us = 0;
        s_line[line].timer_rounds = 0;
        s_line[line].timer_ringing = false;
        activity_log(had ? "Line %d: timer cancelled" : "Line %d: no timer to cancel", line);
    }
    play_transient(line, TONE_CONFIRM, 700, TEL_TIMER_ENTRY, TEL_TIMER_ENTRY);
}

/* The call is over: start counting. */
static void timer_start(int line)
{
    s_line[line].timer_accepted = false;
    int minutes = s_line[line].timer_minutes;
    if (minutes <= 0) return;
    s_line[line].timer_end_us = esp_timer_get_time() + (int64_t)minutes * 60 * 1000000;
    s_line[line].timer_rounds = 0;
    s_line[line].timer_ringing = false;
    activity_log("Line %d: timer set for %d minute%s", line, minutes, minutes == 1 ? "" : "s");
}

/* The caller hung up while entering or just after: use what was dialled. */
static void timer_hung_up(int line)
{
    if (s_line[line].entry_alarm) {   /* an alarm is set the moment it is accepted: nothing to finish */
        s_line[line].timer_accepted = false;
        s_line[line].timer_n = 0;
        return;
    }
    if (!s_line[line].timer_accepted) {
        if (s_line[line].timer_n < 1) return;
        s_line[line].timer_minutes = atoi(s_line[line].timer_digits);
        s_line[line].timer_n = 0;
        if (s_line[line].timer_minutes <= 0) {   /* 0 = cancel */
            s_line[line].timer_end_us = 0;
            s_line[line].timer_rounds = 0;
            s_line[line].timer_ringing = false;
            activity_log("Line %d: timer cancelled", line);
            return;
        }
    }
    timer_start(line);
}

/* ---- alarm (dial 256) ------------------------------------------------------
 * Four digits, hhmm on a 24 hour clock, local time; "99" cancels. The time is
 * set as soon as it's accepted (it is an absolute time, so there's nothing to
 * wait for) and kept in flash, so it survives a restart. It rings once, at the
 * next hh:mm. The ring is longer and lasts longer than the timer's, and the
 * message that follows is the alarm's own. */
#define ALARM_RING_SECS    12
#define ALARM_RING_ROUNDS  8
#define ALARM_RING_EVERY_US (25LL * 1000000)
#define ALARM_LATE_S       600     /* rings if the board was off at the time, up to this late */
#define ALARM_NVS_NS       "telalarm"

static bool clock_is_set(void) { return time(NULL) > 1700000000; }

static void alarm_save(int line)
{
    nvs_handle_t h;
    if (nvs_open(ALARM_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i64(h, line ? "a1" : "a0", (int64_t)s_line[line].alarm_at);
    nvs_commit(h);
    nvs_close(h);
}

time_t telephony_alarm_get(int line)
{
    return (line >= 0 && line < NLINES) ? s_line[line].alarm_at : 0;
}

/* The caller finished dialling (or "99"): set or cancel, then confirm and hang up on them like the timer. */
static void alarm_accept(int line)
{
    char *d = s_line[line].timer_digits;
    s_line[line].timer_n = 0;
    if (d[0] == '9' && d[1] == '9') {
        bool had = s_line[line].alarm_at != 0;
        s_line[line].alarm_at = 0;
        s_line[line].alarm_ringing = false;
        s_line[line].alarm_rounds = 0;
        alarm_save(line);
        activity_log(had ? "Line %d: alarm cancelled" : "Line %d: no alarm to cancel", line);
    } else {
        int hh = (d[0] - '0') * 10 + (d[1] - '0'), mm = (d[2] - '0') * 10 + (d[3] - '0');
        if (hh > 23 || mm > 59 || !clock_is_set()) {
            activity_log(hh > 23 || mm > 59 ? "Line %d: alarm: %02d%02d isn't a time on a 24 hour clock"
                                            : "Line %d: alarm: the clock isn't set yet (needs Wi-Fi once)", line,
                         hh, mm);
            play_transient(line, TONE_REORDER, 1500, TEL_REORDER, TEL_DIALTONE);
            return;
        }
        time_t now = time(NULL);
        struct tm lt;
        localtime_r(&now, &lt);
        lt.tm_hour = hh; lt.tm_min = mm; lt.tm_sec = 0; lt.tm_isdst = -1;
        time_t at = mktime(&lt);
        bool tomorrow = at <= now;
        if (tomorrow) { lt.tm_mday++; lt.tm_isdst = -1; at = mktime(&lt); }
        s_line[line].alarm_at = at;
        s_line[line].alarm_ringing = false;
        s_line[line].alarm_rounds = 0;
        alarm_save(line);
        activity_log("Line %d: alarm set for %02d:%02d %s", line, hh, mm, tomorrow ? "tomorrow" : "today");
    }
    s_line[line].timer_accepted = true;
    s_line[line].timer_hangup_us = esp_timer_get_time() + (int64_t)TIMER_HANGUP_GRACE_MS * 1000;
    play_transient(line, TONE_CONFIRM, 700, TEL_TIMER_ENTRY, TEL_TIMER_ENTRY);
}

static void alarm_digit(int line, char ch)
{
    if (s_line[line].timer_accepted || ch < '0' || ch > '9') return;
    if (s_line[line].timer_n == 0) {
        player_stop(line);
        audio_tx_silence(line);
    }
    if (s_line[line].timer_n < 4) {
        s_line[line].timer_digits[s_line[line].timer_n++] = ch;
        s_line[line].timer_digits[s_line[line].timer_n] = '\0';
    }
    s_line[line].last_digit_us = esp_timer_get_time();
    s_line[line].timer_wait_until_us = esp_timer_get_time() + 10LL * 1000000;   /* the next digit */
    if (s_line[line].timer_n == 2 && !strncmp(s_line[line].timer_digits, "99", 2)) alarm_accept(line);
    else if (s_line[line].timer_n >= 4) alarm_accept(line);
}

static void alarm_tick(int line, int64_t now)
{
    if (s_line[line].alarm_announce && s_line[line].state == TEL_DIALTONE) {
        s_line[line].alarm_announce = false;
        audio_tx_silence(line);
        if (player_play(line, ALARM_DONE_NAME, clip_done_cb, NULL) == ESP_OK) {
            s_line[line].state = TEL_PLAYING;
        } else {
            play_transient(line, TONE_CONFIRM, 700, TEL_DIALTONE, TEL_DIALTONE);
        }
        return;
    }
    if (!s_line[line].alarm_at || !clock_is_set()) return;
    time_t wall = time(NULL);
    if (wall < s_line[line].alarm_at) return;
    if (!s_line[line].alarm_ringing && wall - s_line[line].alarm_at > ALARM_LATE_S) {
        activity_log("Line %d: the alarm time passed while the board was off", line);
        s_line[line].alarm_at = 0;
        alarm_save(line);
        return;
    }
    tel_state_t st = s_line[line].state;
    if (st == TEL_IDLE && !line_offhook(line)) {
        if (!s_line[line].alarm_ringing) {
            s_line[line].alarm_ringing = true;
            s_line[line].alarm_rounds = ALARM_RING_ROUNDS;
            s_line[line].alarm_next_us = now;
            activity_log("Line %d: alarm", line);
            webhook_eventf("alarm", line, "ring");
        }
        if (now >= s_line[line].alarm_next_us) {
            if (s_line[line].alarm_rounds == 0) {
                s_line[line].alarm_at = 0;   /* nobody answered: give up */
                s_line[line].alarm_ringing = false;
                alarm_save(line);
            } else {
                telephony_bell(line, ALARM_RING_SECS, false);   /* rings even in quiet hours: it was asked for */
                s_line[line].alarm_next_us = now + ALARM_RING_EVERY_US;
                s_line[line].alarm_rounds--;
            }
        }
    } else if (st == TEL_DIALTONE) {
        bool was_ringing = s_line[line].alarm_ringing;
        s_line[line].alarm_at = 0;
        s_line[line].alarm_ringing = false;
        s_line[line].alarm_rounds = 0;
        alarm_save(line);
        if (!was_ringing) {
            activity_log("Line %d: alarm", line);
            webhook_eventf("alarm", line, "ring");
        }
        s_line[line].alarm_announce = true;
    }
}

static void timer_digit(int line, char ch)
{
    if (s_line[line].entry_alarm) { alarm_digit(line, ch); return; }
    if (s_line[line].timer_accepted) return;   /* already have the minutes */
    if (ch == '#' && s_line[line].timer_n >= 1) { timer_accept(line, TIMER_HANGUP_GRACE_MS); return; }   /* touch-tone "enter" */
    if (ch < '0' || ch > '9') return;
    if (s_line[line].timer_n == 0) {
        player_stop(line);   /* dialled over the prompt: no need to hear the rest */
        audio_tx_silence(line);
    }
    if (s_line[line].timer_n < 2) {
        s_line[line].timer_digits[s_line[line].timer_n++] = ch;
        s_line[line].timer_digits[s_line[line].timer_n] = '\0';
    }
    s_line[line].last_digit_us = esp_timer_get_time();
    if (s_line[line].timer_n >= 2) timer_accept(line, TIMER_HANGUP_GRACE_MS);
}

/* Called every tick for `line`: waits for digits while asking, and rings when
 * a running timer is up. */
static void timer_tick(int line, int64_t now)
{
    if (s_line[line].timer_announce && s_line[line].state == TEL_DIALTONE) {
        s_line[line].timer_announce = false;
        audio_tx_silence(line);
        if (player_play(line, TIMER_DONE_NAME, clip_done_cb, NULL) == ESP_OK) {
            s_line[line].state = TEL_PLAYING;
        } else {
            play_transient(line, TONE_CONFIRM, 700, TEL_DIALTONE, TEL_DIALTONE);
        }
        return;
    }
    if (s_line[line].state == TEL_TIMER_ENTRY) {
        if (s_line[line].timer_accepted) {
            if (now >= s_line[line].timer_hangup_us) {
                /* The minutes are in and nobody hung up: the line hangs up on the caller
                 * (fast busy until the handset goes down) and the countdown starts. */
                if (!s_line[line].entry_alarm) timer_start(line);
                else s_line[line].timer_accepted = false;
                play_persistent(line, TONE_REORDER, TEL_REORDER);
            }
        } else if (s_line[line].entry_alarm) {
            if (s_line[line].timer_wait_until_us && now >= s_line[line].timer_wait_until_us) {
                s_line[line].timer_wait_until_us = 0;
                s_line[line].timer_n = 0;
                activity_log("Line %d: alarm: no time entered", line);
                play_transient(line, TONE_REORDER, 1500, TEL_REORDER, TEL_DIALTONE);
            }
        } else if (s_line[line].timer_n == 1 &&
            (now - s_line[line].last_digit_us) / 1000 >= TIMER_SECOND_DIGIT_WAIT_MS) {
            timer_accept(line, 0);   /* one digit is enough (1-9 minutes), and the 3 s of waiting has passed */
        } else if (s_line[line].timer_n == 0 && s_line[line].timer_wait_until_us &&
                   now >= s_line[line].timer_wait_until_us) {
            s_line[line].timer_wait_until_us = 0;
            activity_log("Line %d: timer: no minutes entered", line);
            play_transient(line, TONE_REORDER, 1500, TEL_REORDER, TEL_DIALTONE);
        }
        return;
    }
    if (!s_line[line].timer_end_us || now < s_line[line].timer_end_us) return;
    tel_state_t st = s_line[line].state;
    if (st == TEL_IDLE && !line_offhook(line)) {
        /* On-hook: ring the bell a few times, until someone picks up. The
         * timer stays set through the last round, so answering the last ring
         * still hears the message. */
        if (!s_line[line].timer_ringing) {
            s_line[line].timer_ringing = true;
            s_line[line].timer_rounds = TIMER_RING_ROUNDS;
            s_line[line].timer_next_us = now;
            activity_log("Line %d: timer is up", line);
            webhook_eventf("timer", line, "done");
        }
        if (now >= s_line[line].timer_next_us) {
            if (s_line[line].timer_rounds == 0) {
                s_line[line].timer_end_us = 0;   /* nobody answered: give up */
                s_line[line].timer_ringing = false;
            } else {
                telephony_bell(line, TIMER_RING_SECS, false);   /* rings even in quiet hours: someone asked for it */
                s_line[line].timer_next_us = now + TIMER_RING_EVERY_US;
                s_line[line].timer_rounds--;
            }
        }
    } else if (st == TEL_DIALTONE) {
        /* Handset is up and idle: the message (or a beep), once. */
        bool was_ringing = s_line[line].timer_ringing;
        s_line[line].timer_end_us = 0;
        s_line[line].timer_rounds = 0;
        s_line[line].timer_ringing = false;
        if (!was_ringing) {
            activity_log("Line %d: timer is up", line);
            webhook_eventf("timer", line, "done");
        }
        s_line[line].timer_announce = true;   /* spoken message (or a beep if there isn't one) */
    }
    /* Any other state (dialling, on a call, ...): try again next tick. */
}

static void handle_action(int line, const dialplan_action_t *a)
{
    /* Every code, before it runs: Home Assistant automations key on the
     * digits (webhook.h), whatever the code does on the board. */
    if (a->kind != ACT_NONE)
        webhook_event(a->kind == ACT_WRONG_NUMBER ? "wrong_number" : "dial", line,
                      s_line[line].coll.buf);
    /* A spoken digit may still be playing (readback); it mustn't block what's next. */
    if (a->kind != ACT_NONE && s_line[line].rb_clip) {
        player_stop_and_wait(line, 150);
        s_line[line].rb_clip = false;
    }
    switch (a->kind) {
        case ACT_NONE:
            return;

        case ACT_PI_DIGITS:
            audio_tx_silence(line);
            s_line[line].seq = PI_DIGITS;
            s_line[line].seq_i = 0;
            s_line[line].seq_playing = false;
            s_line[line].seq_next_us = esp_timer_get_time() + 1000000;   /* a moment of quiet first */
            s_line[line].state = TEL_PLAYING;
            activity_log("Line %d: reading pi aloud", line);
            break;

        case ACT_ALARM:
            audio_tx_silence(line);
            s_line[line].entry_alarm = true;
            s_line[line].timer_n = 0;
            s_line[line].timer_accepted = false;
            s_line[line].timer_digits[0] = '\0';
            s_line[line].timer_wait_until_us = esp_timer_get_time() + 30LL * 1000000;
            if (player_play(line, ALARM_PROMPT_NAME, clip_done_cb, NULL) == ESP_OK) {
                s_line[line].state = TEL_TIMER_ENTRY;
            } else {
                s_line[line].timer_wait_until_us = esp_timer_get_time() + TIMER_FIRST_DIGIT_WAIT_US;
                play_transient(line, TONE_ANSWERING_BEEP, 300, TEL_TIMER_ENTRY, TEL_TIMER_ENTRY);
            }
            break;

        case ACT_TIMER:
            audio_tx_silence(line);
            s_line[line].entry_alarm = false;
            s_line[line].timer_n = 0;
            s_line[line].timer_accepted = false;
            s_line[line].timer_digits[0] = '\0';
            s_line[line].timer_wait_until_us = esp_timer_get_time() + 30LL * 1000000;
            if (player_play(line, TIMER_PROMPT_NAME, clip_done_cb, NULL) == ESP_OK) {
                s_line[line].state = TEL_TIMER_ENTRY;
            } else {
                /* No prompt clip on this board: a short beep, then wait for the minutes. */
                s_line[line].timer_wait_until_us = esp_timer_get_time() + TIMER_FIRST_DIGIT_WAIT_US;
                play_transient(line, TONE_ANSWERING_BEEP, 300, TEL_TIMER_ENTRY, TEL_TIMER_ENTRY);
            }
            break;

        case ACT_SET_MODE:
            dialplan_apply_mode(line, (dialplan_mode_change_t)a->arg);
            play_transient(line, TONE_CONFIRM, 700, TEL_DIALTONE, TEL_DIALTONE);
            break;

        case ACT_OPERATOR_HELP: {
            /* A two-line board's built-in prompt also mentions 321 (call the
             * other phone); an uploaded or generated operator.wav wins. */
            const char *op = "operator.wav";
            if (board_hw_config_get().num_lines >= 2 && !storage_has_own(op)) op = "operator_2line.wav";
            esp_err_t pe = player_play(line, op, clip_done_cb, NULL);
            if (pe != ESP_OK && op[8] == '_') { op = "operator.wav"; pe = player_play(line, op, clip_done_cb, NULL); }
            if (pe == ESP_OK) {
                s_line[line].state = TEL_PLAYING;
                activity_log("Line %d: played %s", line, op);
            } else {
                ESP_LOGI(TAG, "line %d: operator help (no operator.wav in storage yet)", line);
                play_transient(line, TONE_CONFIRM, 700, TEL_DIALTONE, TEL_DIALTONE);
            }
            break;
        }

        case ACT_PLAY_CLIP: /* dial-code entry (Settings -> Dial Codes), a->filename set directly */
            if (play_clip_any_ext(line, a->filename) == ESP_OK) {
                s_line[line].state = TEL_PLAYING;
                activity_log("Line %d: played %s", line, a->filename);
            } else {
                ESP_LOGI(TAG, "line %d: play '%s' (not in storage?)", line, a->filename);
                play_transient(line, TONE_CONFIRM, 700, TEL_DIALTONE, TEL_DIALTONE);
            }
            break;

        case ACT_GPIO_OUT: {
            /* A pin dial code (Settings -> Dial Codes -> Pin outputs). a->arg
             * is the GPIO, a->arg2 the seconds high (0 = toggle). With no pin
             * it's a Home Assistant code: the "dial" event sent above is the
             * whole job, so just confirm. */
            if (a->arg == DIALPLAN_PIN_NONE) {
                activity_log("Line %d: dial code %s for Home Assistant", line, s_line[line].coll.buf);
                play_transient(line, TONE_CONFIRM, 400, TEL_DIALTONE, TEL_DIALTONE);
                break;
            }
            esp_err_t pr = auxpins_pulse(a->arg, a->arg2);
            if (pr == ESP_OK) {
                activity_log("Line %d: dial code -> GPIO%d %s", line, a->arg,
                             a->arg2 ? "high" : "toggled");
                play_transient(line, TONE_CONFIRM, 400, TEL_DIALTONE, TEL_DIALTONE);
            } else {
                ESP_LOGW(TAG, "line %d: pin code for GPIO%d not run (%s)", line, a->arg,
                         esp_err_to_name(pr));
                play_transient(line, TONE_REORDER, 700, TEL_DIALTONE, TEL_DIALTONE);
            }
            break;
        }

        case ACT_RECORD_ARCHIVE: /* 211: prompt (if configured) -> beep -> record */
            s_line[line].rec_purpose = REC_ARCHIVE;
            if (player_play(line, RECORD_PROMPT_NAME, clip_done_cb, NULL) == ESP_OK) {
                s_line[line].state = TEL_PLAYING_RECORD_PROMPT;
            } else {
                /* No record_prompt.wav generated yet -- normal, silent
                 * fallback straight to the beep, not an error. */
                start_record_beep(line);
            }
            break;

        case ACT_RECORD_GREETING: /* 212: straight to beep -- no prompt clip,
                                    * dialing 212 on purpose already says
                                    * "record now," nothing to instruct. */
            s_line[line].rec_purpose = REC_GREETING;
            start_record_beep(line);
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
                activity_log("Line %d: dialing %s", line, a->number);
            } else {
                ESP_LOGW(TAG, "line %d: VoIP dial %s -> %s (reorder)",
                         line, a->number, esp_err_to_name(r));
                play_persistent(line, TONE_REORDER, TEL_REORDER);
            }
            break;
        }

        case ACT_HOME_CALL: {
            /* Another board on this network (homecall.h): a direct SIP call,
             * then the same path as a VoIP call through a server. */
            strncpy(s_line[line].last_number, a->number, sizeof(s_line[line].last_number) - 1);
            char ip[16], name[32];
            esp_err_t r = homecall_lookup(a->number, ip, sizeof(ip), name, sizeof(name))
                              ? voip_dial_local(line, ip, a->number) : ESP_ERR_NOT_FOUND;
            if (r == ESP_OK) {
                audio_tx_tone(line, TONE_RINGBACK);
                s_line[line].state = TEL_RINGBACK;
                s_line[line].peer = PEER_VOIP;
                activity_log("Line %d: calling %s (%s), board to board", line, a->number, name);
            } else {
                ESP_LOGW(TAG, "line %d: home call %s -> %s (reorder)", line, a->number, esp_err_to_name(r));
                activity_log(r == ESP_ERR_NOT_FOUND
                                 ? "Line %d: can't call %s: that board isn't on the network now"
                                 : !homecall_number(line)[0]
                                 ? "Line %d: can't call %s: give this line a board-to-board number first"
                                 : "Line %d: can't call %s right now", line, a->number);
                play_persistent(line, TONE_REORDER, TEL_REORDER);
            }
            break;
        }

        case ACT_BT_DIAL: {
            /* Only reachable on the Bluetooth line with the module enabled
             * (process_digit() gates dialplan_set_bt_dialout()). The call's
             * own audio -- network ringback, then the far end -- comes back
             * over SCO, so the line goes straight to passthrough instead of
             * playing a local ringback that would fight the BM64's output. */
            strncpy(s_line[line].last_number, a->number, sizeof(s_line[line].last_number) - 1);
            esp_err_t r = bt_companion_get_status().bt_connected
                              ? bt_companion_make_call(a->number) : ESP_ERR_INVALID_STATE;
            if (r == ESP_OK) {
                activity_log("Line %d: dialing %s via Bluetooth", line, a->number);
                enter_bt_passthrough(line);
            } else {
                ESP_LOGW(TAG, "line %d: Bluetooth dial %s -> %s (reorder)",
                         line, a->number, esp_err_to_name(r));
                activity_log("Line %d: can't dial %s -- no phone connected over Bluetooth",
                             line, a->number);
                play_persistent(line, TONE_REORDER, TEL_REORDER);
            }
            break;
        }

        case ACT_WRONG_NUMBER:
            ESP_LOGI(TAG, "line %d: wrong number '%s'", line, s_line[line].coll.buf);
            activity_log("Line %d: wrong number '%s'", line, s_line[line].coll.buf);
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
    if (st == TEL_INCALL) {
        in_call_key(line, ch);
        return;
    }
    if (st == TEL_TIMER_ENTRY) {
        timer_digit(line, ch);
        return;
    }
    if (st != TEL_DIALTONE && st != TEL_DIALING) {
        return; /* ignore digits unless we're expecting them */
    }
    if (st == TEL_DIALTONE || s_line[line].dialout_tone) {
        audio_tx_silence(line); /* stop dial tone on first digit (or the dial-out tone on the next) */
        s_line[line].dialout_tone = false;
    }
    s_line[line].state = TEL_DIALING;
    s_line[line].dialing = true;
    s_line[line].last_digit_us = esp_timer_get_time();

    /* optional digit readback: the digit spoken (0.wav..9.wav), or a short
     * touch-tone burst without them; collection is unaffected */
    if (dialplan_modes()->readback_digits) {
        readback_digit(line, ch);
    }

    bool bt_out = line == bt_companion_line() && bt_companion_active() && bt_companion_can_dial();
    dialplan_set_bt_dialout(bt_out);
    dialplan_action_t a = dialplan_feed(&s_line[line].coll, line, ch);
    if (bt_out && a.kind == ACT_NONE && s_line[line].coll.len == 1 &&
        s_line[line].coll.buf[0] == DIALPLAN_BT_PREFIX) {
        /* Dialed 9 for an outside line: dial tone pulses, then steady again. */
        audio_tx_tone(line, TONE_DIAL_OUT);
        s_line[line].dialout_tone = true;
    }
    ESP_LOGD(TAG, "line %d: digit '%c' -> collected '%s', action kind=%d", line,
             ch, s_line[line].coll.buf, a.kind);
    if (a.kind != ACT_NONE) {
        handle_action(line, &a);
    }
}

/* True while `line` is mid-answering-machine-activity (playing its
 * greeting, mid-beep, or actually recording) -- shared by both barge-in
 * branches in on_offhook() below. */
static bool am_active(int line)
{
    return s_line[line].rec_purpose == REC_VOICEMAIL &&
        (s_line[line].state == TEL_PLAYING_RECORD_PROMPT ||
         s_line[line].state == TEL_RECORD_BEEP ||
         s_line[line].state == TEL_RECORDING);
}

/* ---- main event handlers ---- */
static void on_offhook(int line)
{
    if (s_line[line].timer_end_us && esp_timer_get_time() >= s_line[line].timer_end_us) {
        /* Picked up the ringing timer: say so once the line is on dial tone. */
        s_line[line].timer_end_us = 0;
        s_line[line].timer_rounds = 0;
        s_line[line].timer_ringing = false;
        s_line[line].timer_announce = true;
    }
    if (s_line[line].alarm_at && clock_is_set() && time(NULL) >= s_line[line].alarm_at) {
        /* Picked up the ringing alarm. */
        s_line[line].alarm_at = 0;
        s_line[line].alarm_rounds = 0;
        s_line[line].alarm_ringing = false;
        s_line[line].alarm_announce = true;
        alarm_save(line);
    }
    if (bt_companion_should_intercept(line)) {
        bool music_only = !bt_companion_call_live();
        activity_log(music_only ? "Line %d: picked up while the paired phone plays audio"
                                : "Line %d: picked up the paired phone's call", line);
        enter_bt_passthrough(line);
        s_line[line].bt_music_pass = music_only;
        s_line[line].bt_music_seen_us = esp_timer_get_time();
        return;
    }
    tel_state_t st = s_line[line].state;
    if (st == TEL_RINGING && s_line[line].peer == PEER_BT) {
        /* Lifting the handset answers the paired phone's call, just as a
         * Bluetooth headset would. ag1170 has already silenced the bell (its
         * ring-trip path stops ringing on off-hook). */
        bt_companion_accept_call();
        activity_log("Line %d: answered Bluetooth call%s%s", line,
                     s_line[line].caller_number[0] ? " from " : "", s_line[line].caller_number);
        enter_bt_passthrough(line);
        return;
    }
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
            /* Barge-in: peer might be running the answering machine on OUR
             * behalf right now (an unanswered call to us that timed out and
             * got auto-answered -- see auto_answer(), which deliberately
             * leaves `line` sitting in TEL_RINGING with peer still set so a
             * real pickup lands exactly here instead of nowhere). */
            if (am_active(peer) && s_line[peer].state == TEL_RECORDING) {
                /* Mid-recording: recorder's done callback reliably fires
                 * even on an explicit stop (unlike player's, below), so
                 * defer the bridge to IN_REC_DONE instead of racing
                 * recorder.c's teardown. */
                s_line[peer].am_bargein = true;
                recorder_stop(peer);
            } else if (am_active(peer)) {
                /* Still playing the greeting, or mid-beep -- player_stop()
                 * does NOT reliably fire IN_CLIP_DONE on an explicit stop
                 * (finish()'s call_cb is tied to whether playback actually
                 * ran to completion), so there's nothing to defer to;
                 * bridge immediately instead of waiting for an event that
                 * might never come. */
                player_stop(peer);
                s_line[peer].tone_return_us = 0; /* in case the beep's own timer is what's running */
                s_line[peer].rec_purpose = REC_NONE;
                connect_bridge(line, peer);
            } else {
                connect_bridge(line, peer);
            }
        } else {
            enter_dialtone(line);
        }
        return;
    }
    /* Barge-in: this line itself was auto-answered as the machine for an
     * incoming VoIP call (state already moved off RINGING) -- a real pickup
     * now means take the call live instead. voip_answer() already ran
     * during auto-answer, so the SIP side stays as-is -- just switch what's
     * happening locally from machine to live audio. */
    if (am_active(line) && s_line[line].peer == PEER_VOIP) {
        if (st == TEL_RECORDING) recorder_stop(line); else player_stop(line);
        s_line[line].rec_purpose = REC_NONE;
        audio_set_uplink_inject(line, false); /* real mic reaches the far end again, not this line's own TX */
        s_line[line].state = TEL_INCALL;
        activity_log("Line %d: picked up during answering machine -- live", line);
        return;
    }
    /* normal pickup -- after an unacknowledged brownout restart, the first
     * pickup hears the "battery running flat" tone, then dial tone. */
    if (board_power_warning_announce()) {
        play_transient(line, TONE_LOW_POWER, TONE_LOW_POWER_MS, TEL_REORDER, TEL_DIALTONE);
        return;
    }
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
    /* Hanging up right after the minutes counts as "done": a single digit
     * would otherwise wait out its 3 seconds and be lost. */
    if (s_line[line].state == TEL_TIMER_ENTRY) timer_hung_up(line);
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
    activity_log("Line %d: ringing -- caller %s <%s>", line,
                  s_line[line].caller_name[0] ? s_line[line].caller_name : "(no name)",
                  s_line[line].caller_number);
    webhook_eventf("ring", line, "%s <%s>", s_line[line].caller_name, s_line[line].caller_number);
    s_line[line].state = TEL_RINGING;
    s_line[line].peer = PEER_VOIP;
    start_ring(line, false);
}

/* The paired phone's call state changed (or a caller ID arrived for a call
 * already ringing). Only line 0 carries Bluetooth audio, see
 * bt_companion_line(). */
static void on_bt_call(int line, bt_call_state_t cs)
{
    const char *cid = bt_companion_get_status().caller_id;
    tel_state_t st = s_line[line].state;
    bool ringing_for_bt = (st == TEL_RINGING && s_line[line].peer == PEER_BT);

    if (cs == BT_CALL_INCOMING) {
        if (ringing_for_bt) {
            /* Already ringing -- a caller ID that arrived after the ring. */
            if (cid[0]) strlcpy(s_line[line].caller_number, cid, sizeof(s_line[line].caller_number));
            return;
        }
        if (st != TEL_IDLE || line_offhook(line)) {
            /* Line busy, or the handset is already up (e.g. mid-call with the
             * other connected device, or listening to music) -- an off-hook
             * phone can't be rung. The ringing device rings on its own. */
            ESP_LOGI(TAG, "line %d: Bluetooth call incoming but line busy (%s) -- not ringing",
                     line, telephony_state_name(st));
            return;
        }
        s_line[line].caller_name[0] = '\0';
        strlcpy(s_line[line].caller_number, cid, sizeof(s_line[line].caller_number));
        ESP_LOGI(TAG, "line %d: ringing -- Bluetooth call%s%s", line, cid[0] ? " from " : "", cid);
        activity_log("Line %d: ringing -- Bluetooth call%s%s", line, cid[0] ? " from " : "", cid);
        webhook_eventf("ring", line, "Bluetooth %s", cid);
        s_line[line].state = TEL_RINGING;
        s_line[line].peer = PEER_BT;
        start_ring(line, false);
        return;
    }

    /* No longer ringing anywhere. The merged state (see bt_companion.h) could
     * have moved to anything -- answered on the phone, declined, caller gave
     * up, or another connected device's call now being the one reported --
     * and in every case the bell must stop. */
    if (ringing_for_bt) {
        ag1170_ring_stop(line);
        if (cs == BT_CALL_ACTIVE || cs == BT_CALL_ACTIVE_WAITING || cs == BT_CALL_ACTIVE_HOLD) {
            /* Answered on the phone itself. The call stays there; lifting the
             * handset afterwards still picks it up, since a live connection
             * routes pickup into passthrough. */
            activity_log("Line %d: Bluetooth call answered on the phone", line);
        } else {
            activity_log("Line %d: missed Bluetooth call%s%s", line,
                         s_line[line].caller_number[0] ? " from " : "", s_line[line].caller_number);
        }
        enter_idle(line);
        return;
    }
    if (cs == BT_CALL_IDLE && st == TEL_BT_PASSTHROUGH) {
        /* Far end hung up, the dial-out failed, or the call was moved back
         * to the phone. Like the phone network: reorder until hang-up. SCO
         * is down by now, so the codec has the Vin node to itself again. */
        activity_log("Line %d: Bluetooth call ended", line);
        play_persistent(line, TONE_REORDER, TEL_REORDER);
        return;
    }
    if (bt_companion_call_live() && (st == TEL_DIALTONE || st == TEL_DIALING)) {
        /* A call started from the phone itself while this handset was up
         * (dialing on the phone with the audio routed here). */
        activity_log("Line %d: picked up the paired phone's call", line);
        enter_bt_passthrough(line);
    }
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
                case IN_OFFHOOK:        webhook_event("offhook", e.line, NULL);
                                        callcap_hook(e.line, true);   /* before the dial tone starts */
                                        on_offhook(e.line); break;
                case IN_ONHOOK:         webhook_event("onhook", e.line, NULL);
                                        callcap_hook(e.line, false);
                                        on_onhook(e.line);  break;
                case IN_DIGIT:          process_digit(e.line, e.digit); break;
                case IN_VOIP_INCOMING:  on_voip_incoming(e.line); break;
                case IN_BT_CALL:        on_bt_call(e.line, (bt_call_state_t)e.digit); break;
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
                        /* If we were still ringing (incoming call closed --
                         * caller hung up/CANCELed -- before anyone picked
                         * up), the SLIC's bell is only ever silenced by
                         * ag1170_ring_stop() or an actual answer (service_hook()'s
                         * ring-trip path). Neither happens here on its own --
                         * without this, the physical bell keeps ringing
                         * indefinitely even though the FSM below moves on. */
                        if (s_line[e.line].state == TEL_RINGING)
                            ag1170_ring_stop(e.line);
                        if (am_active(e.line)) {
                            /* Caller hung up mid-greeting/beep/recording on
                             * an auto-answered call -- end_call() stops
                             * whatever's in flight (recorder's done callback
                             * still fires after recorder_stop(), but
                             * IN_REC_DONE's own state-changed guard above
                             * makes that a no-op once this has already run). */
                            end_call(e.line);
                        } else if (line_offhook(e.line)) {
                            enter_dialtone(e.line);
                        } else {
                            enter_idle(e.line);
                        }
                    }
                    break;
                case IN_CLIP_DONE:
                    /* Ignore if we've already moved on (e.g. hung up mid-clip --
                     * end_call() already handled state via player_stop()). */
                    if (s_line[e.line].state == TEL_PLAYING && s_line[e.line].seq &&
                        s_line[e.line].seq_i < strlen(s_line[e.line].seq)) {
                        s_line[e.line].seq_playing = false;
                        s_line[e.line].seq_next_us = esp_timer_get_time() +
                            seq_gap_us(s_line[e.line].seq, s_line[e.line].seq_i);
                    } else if (s_line[e.line].state == TEL_PLAYING) {
                        s_line[e.line].seq = NULL;
                        if (line_offhook(e.line)) enter_dialtone(e.line);
                        else enter_idle(e.line);
                    } else if (s_line[e.line].state == TEL_TIMER_ENTRY) {
                        /* The question has been asked: wait for the minutes. */
                        s_line[e.line].timer_wait_until_us = esp_timer_get_time() + TIMER_FIRST_DIGIT_WAIT_US;
                    } else if (s_line[e.line].state == TEL_PLAYING_RECORD_PROMPT) {
                        /* The "leave a message after the beep"-style clip
                         * (211's record_prompt.wav, or an answering
                         * machine's own greeting) finished -- continue into
                         * the beep, don't just drop back to dial tone like a
                         * normal clip. REC_VOICEMAIL bypasses the hook check
                         * for the same reason as the tone-return dispatch
                         * above -- no local human's hook state applies. */
                        if (s_line[e.line].rec_purpose == REC_VOICEMAIL || line_offhook(e.line)) {
                            start_record_beep(e.line);
                        } else {
                            enter_idle(e.line);
                        }
                    }
                    break;
                case IN_REC_DONE: {
                    int l = e.line;
                    /* Tell Home Assistant what was recorded; diagnostics mic
                     * tests don't come through here. */
                    if (e.digit && e.kind != REC_NONE) {
                        const char *kind = e.kind == REC_GREETING  ? "greeting"
                                         : e.kind == REC_VOICEMAIL ? "voicemail"
                                                                   : "message";
                        if (e.kind != REC_GREETING) note_new_message();
                        webhook_eventf("recording", l, "%s %s", kind, s_line[l].rec_name);
                    }
                    if (s_line[l].state != TEL_RECORDING) break; /* already moved on */
                    if (s_line[l].rec_purpose == REC_VOICEMAIL) {
                        int called = s_line[l].peer; /* local case only -- PEER_VOIP for the VoIP case */
                        if (s_line[l].am_bargein && called >= 0 && called < NLINES &&
                            line_offhook(called)) {
                            /* Local-peer barge-in (on_offhook()) stopped this
                             * recording mid-flight and is waiting for THIS
                             * teardown to actually finish before it's safe
                             * to bridge -- see am_active()'s doc comment. */
                            s_line[l].rec_purpose = REC_NONE;
                            s_line[l].am_bargein = false;
                            connect_bridge(called, l);
                        } else {
                            end_call(l); /* voip_hangup() for a VoIP peer; TEL_RINGING-peer cleanup for local */
                        }
                    } else if (line_offhook(l)) {
                        enter_dialtone(l);
                    } else {
                        enter_idle(l);
                    }
                    break;
                }
            }
        }

        int64_t now = esp_timer_get_time();
        for (int line = 0; line < NLINES_ACTIVE; line++) {
            timer_tick(line, now);
            alarm_tick(line, now);
            /* inter-digit timeout */
            if (s_line[line].dialing &&
                (now - s_line[line].last_digit_us) / 1000 >= dialplan_interdigit_ms()) {
                dialplan_set_bt_dialout(line == bt_companion_line() && bt_companion_active() &&
                                        bt_companion_can_dial());
                dialplan_action_t a = dialplan_timeout(&s_line[line].coll, line);
                s_line[line].dialing = false;
                if (a.kind != ACT_NONE) handle_action(line, &a);
            }
            /* Bluetooth music: it reaches the earpiece straight from the module, so the handset
             * joins it (no dial tone) while it plays, and goes back to dial tone a few seconds
             * after it stops (the gap between two tracks must not flip it). */
            if (line == bt_companion_line() && bt_companion_active() && line_offhook(line)) {
                bool music = bt_companion_music_playing();
                if (music) s_line[line].bt_music_seen_us = now;
                if (music && s_line[line].state == TEL_DIALTONE && s_line[line].coll.len == 0) {
                    activity_log("Line %d: the paired phone is playing audio: handset joins it", line);
                    enter_bt_passthrough(line);
                    s_line[line].bt_music_pass = true;
                } else if (s_line[line].state == TEL_BT_PASSTHROUGH && s_line[line].bt_music_pass &&
                           !music && !bt_companion_call_live() &&
                           now - s_line[line].bt_music_seen_us > 5000000) {
                    activity_log("Line %d: the music stopped: dial tone", line);
                    enter_dialtone(line);
                }
            }
            /* Reading digits aloud: start the next clip when its pause is up. */
            if (s_line[line].seq && s_line[line].state == TEL_PLAYING &&
                !s_line[line].seq_playing && now >= s_line[line].seq_next_us) {
                char name[16];
                digit_clip_name(s_line[line].seq[s_line[line].seq_i], name, sizeof(name));
                bool last = s_line[line].seq_i + 1 >= strlen(s_line[line].seq);
                esp_err_t pe = last ? player_play_now(line, name, clip_done_cb, NULL)
                                    : player_play_chained(line, name, clip_done_cb, NULL);
                if (pe == ESP_OK) {
                    s_line[line].seq_playing = true;
                    s_line[line].seq_i++;
                } else if (pe == ESP_ERR_INVALID_STATE) {
                    s_line[line].seq_next_us = now + 20000;   /* the last clip is still winding down */
                } else {
                    ESP_LOGW(TAG, "line %d: can't read digit clip %s: %s", line, name, esp_err_to_name(pe));
                    s_line[line].seq = NULL;
                    play_transient(line, TONE_CONFIRM, 700, TEL_DIALTONE, TEL_DIALTONE);
                }
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
                else if (ret == TEL_TIMER_ENTRY) {
                    /* The stand-in beep for a missing timer prompt: stop it
                     * (it's a continuous tone) and keep waiting for digits. */
                    if (s_line[line].state == TEL_TIMER_ENTRY) audio_tx_silence(line);
                }
                else if (ret == TEL_RECORDING) {
                    /* The beep just finished -- which start_recording_*()
                     * runs depends on what this beep was actually for (see
                     * rec_purpose_t). REC_VOICEMAIL has no local human on
                     * this line to check hook state against (VoIP: nobody's
                     * physically holding the handset; local peer: this is
                     * the CALLED line, which never went off-hook at all --
                     * see auto_answer()), so it bypasses line_offhook()
                     * entirely, unlike REC_ARCHIVE/REC_GREETING which are
                     * always a real person recording from their own phone. */
                    rec_purpose_t purpose = s_line[line].rec_purpose;
                    if (purpose == REC_VOICEMAIL) {
                        int for_line = (s_line[line].peer == PEER_VOIP) ? line : s_line[line].peer;
                        start_recording_voicemail(line, for_line);
                    } else if (line_offhook(line)) {
                        if (purpose == REC_GREETING) start_recording_greeting(line);
                        else start_recording_archive(line);
                    } else {
                        enter_idle(line);
                    }
                }
            }
            /* Answering machine: after `cfg.rings` unanswered rings, auto-
             * answer. Gated per-line: cfg.enabled must be on for `line`
             * itself, and for a local-peer call cfg.other_line must also be
             * on (routing a family member's unanswered intercom call to
             * voicemail isn't obviously wanted, so it's opt-in separately
             * from VoIP auto-answer -- see board.h). Trigger condition is
             * "cfg.rings full cadence cycles have elapsed" -- simpler and
             * less ambiguous than counting rings-in-progress. During quiet
             * hours, start_ring() already substituted a synthetic
             * always-eligible/zero-rings cfg here, so this fires on the very
             * first tick after ringing "starts" -- see start_ring()'s doc
             * comment. */
            if (s_line[line].state == TEL_RINGING && s_line[line].ring_started_us) {
                board_am_cfg_t cfg = s_line[line].ring_am_cfg;
                /* Never for a Bluetooth call -- the paired phone has its own
                 * voicemail, and answering here would take its call away. */
                bool eligible = cfg.enabled && s_line[line].peer != PEER_BT &&
                    (s_line[line].peer == PEER_VOIP || cfg.other_line);
                if (eligible) {
                    uint16_t on_ms, off_ms;
                    ag1170_ring_cadence_ms(&on_ms, &off_ms);
                    int64_t cycle_us = (int64_t)(on_ms + off_ms) * 1000;
                    if (cycle_us > 0 &&
                        now - s_line[line].ring_started_us >= (int64_t)cfg.rings * cycle_us) {
                        auto_answer(line);
                    }
                }
            }
            /* Handset left off-hook on dial tone -> loud alert (board.h).
             * Gated on still being in DIALTONE, so the armed deadline is
             * self-invalidating: any transition out (dialing, a call, going
             * on-hook) simply stops it from ever being consulted, and coming
             * back to dial tone re-arms it in enter_dialtone(). */
            if (s_line[line].state == TEL_DIALTONE &&
                s_line[line].intercom_at_us &&
                now >= s_line[line].intercom_at_us) {
                s_line[line].intercom_at_us = 0;
                int other = 1 - line;
                if (other >= 0 && other < NLINES_ACTIVE) {
                    if (s_line[other].state == TEL_DIALTONE) {
                        /* Both handsets are up and idle: nobody needs to ring. */
                        ESP_LOGI(TAG, "intercom: line %d and line %d both off-hook, connecting", line, other);
                        audio_tx_silence(other);
                        connect_bridge(line, other);
                    } else {
                        ESP_LOGI(TAG, "intercom: line %d rings line %d", line, other);
                        start_local_call_ex(line, other, FX_NORMAL, true);
                    }
                }
            }
            if (s_line[line].state == TEL_DIALTONE &&
                s_line[line].offhook_alert_at_us &&
                now >= s_line[line].offhook_alert_at_us) {
                s_line[line].offhook_alert_at_us = 0;
                ESP_LOGI(TAG, "line %d: off-hook too long on dial tone -- alert tone", line);
                play_persistent(line, TONE_OFFHOOK_ALERT, TEL_OFFHOOK_ALERT);
            }
        }
        vTaskDelay(tick);
    }
}

void telephony_resume_dialtone(int line)
{
    if (line >= 0 && line < NLINES_ACTIVE && s_line[line].state == TEL_DIALTONE) enter_dialtone(line);
}

/* ---- doorbell ring (telephony_bell) ---- */
static esp_timer_handle_t s_bell_timer[NLINES];

static void bell_stop_cb(void *arg)
{
    int line = (int)(intptr_t)arg;
    /* A real incoming call that started while the bell rang owns the bell
     * now: its own answer/cancel path stops it. */
    if (s_line[line].state != TEL_RINGING) ag1170_ring_stop(line);
}

esp_err_t telephony_bell(int line, int secs, bool respect_quiet)
{
    if (line < 0 || line >= NLINES_ACTIVE || line >= board_hw_config_get().num_lines)
        return ESP_ERR_INVALID_ARG;
    if (secs < 1) secs = 1;
    if (secs > 15) secs = 15;
    if (respect_quiet && board_quiet_hours_active()) return ESP_ERR_NOT_ALLOWED;
    if (line_offhook(line) || s_line[line].state != TEL_IDLE) return ESP_ERR_INVALID_STATE;
    if (!s_bell_timer[line]) {
        const esp_timer_create_args_t a = { .callback = bell_stop_cb, .arg = (void *)(intptr_t)line,
                                            .name = "bell" };
        esp_err_t err = esp_timer_create(&a, &s_bell_timer[line]);
        if (err != ESP_OK) return err;
    }
    esp_timer_stop(s_bell_timer[line]);
    ag1170_ring_start(line);
    return esp_timer_start_once(s_bell_timer[line], (uint64_t)secs * 1000000);
}

/* ---- message waiting ---- */
#define MSG_NVS_NS  "telmsg"
static volatile int s_new_msgs = -1;   /* -1 = not loaded yet */

static void msgs_save(int n)
{
    nvs_handle_t h;
    if (nvs_open(MSG_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_u16(h, "new", (uint16_t)n) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

int telephony_new_messages(void)
{
    if (s_new_msgs < 0) {
        uint16_t v = 0;
        nvs_handle_t h;
        if (nvs_open(MSG_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            nvs_get_u16(h, "new", &v);
            nvs_close(h);
        }
        s_new_msgs = v;
    }
    return s_new_msgs;
}

void telephony_messages_seen(void)
{
    if (telephony_new_messages() == 0) return;
    s_new_msgs = 0;
    msgs_save(0);
}

static void note_new_message(void)
{
    int n = telephony_new_messages();
    if (n < 999) n++;
    s_new_msgs = n;
    msgs_save(n);
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
    /* Alarms set before the last restart. */
    {
        nvs_handle_t h;
        if (nvs_open(ALARM_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            for (int line = 0; line < NLINES; line++) {
                int64_t at = 0;
                nvs_get_i64(h, line ? "a1" : "a0", &at);
                s_line[line].alarm_at = (time_t)at;
            }
            nvs_close(h);
        }
    }
    /* DTMF detector + RX tap only for lines that actually exist. */
    for (int line = 0; line < NLINES_ACTIVE; line++) {
        dtmf_detector_init(&s_line[line].det, audio_sample_rate());
        audio_set_rx_tap(line, dtmf_tap, NULL);
    }

    ag1170_set_event_cb(ag_cb, NULL);
    bt_companion_set_call_cb(bt_call_cb, NULL);
    voip_init(voip_cb, NULL);

    /* Was 4096 -- confirmed live via a "Stack protection fault" panic in this
     * task the first time 211's new record-prompt path called into
     * player_play() from here (player.c's own lookup arrays are heap now,
     * not stack, but this task's other call chains -- storage/dialplan/audio
     * -- were clearly already close to the edge). Headroom, not exact
     * measurement. */
    BaseType_t ok = xTaskCreate(fsm_task, "tel_fsm", 6144, NULL, 5, NULL);
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
