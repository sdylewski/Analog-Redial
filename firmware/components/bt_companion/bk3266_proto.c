/* bk3266_proto.c — see bk3266_proto.h. Pure C, host-testable. */
#include "bk3266_proto.h"

#include <stdio.h>
#include <string.h>

#define HDR0 0x52
#define HDR1 0x42
#define HDR2 0x23
#define FTR0 0x23
#define FTR1 0x42
#define FTR2 0x52

enum { S_H0, S_H1, S_H2, S_LEN, S_BODY, S_F0, S_F1, S_F2 };

size_t bk_build_frame(uint8_t cmd, const uint8_t *data, size_t n, uint8_t *out, size_t out_max)
{
    if (n > BK_FRAME_MAX_DATA) return 0;
    size_t total = 3 + 1 + 1 + n + 1 + 3;
    if (out_max < total) return 0;
    uint8_t len = (uint8_t)(3 + n);
    uint8_t crc = (uint8_t)(len + cmd);
    size_t o = 0;
    out[o++] = HDR0; out[o++] = HDR1; out[o++] = HDR2;
    out[o++] = len;
    out[o++] = cmd;
    for (size_t i = 0; i < n; i++) {
        out[o++] = data[i];
        crc = (uint8_t)(crc + data[i]);
    }
    out[o++] = crc;
    out[o++] = FTR0; out[o++] = FTR1; out[o++] = FTR2;
    return o;
}

size_t bk_build_command(uint8_t code, uint8_t *out, size_t out_max)
{
    return bk_build_frame(BK_MID_SEND, &code, 1, out, out_max);
}

void bk_parser_init(bk_parser_t *p)
{
    memset(p, 0, sizeof(*p));
}

bool bk_parser_feed(bk_parser_t *p, uint8_t b, bk_frame_t *out)
{
    p->bytes++;
    switch (p->state) {
    case S_H0:
        if (b == HDR0) p->state = S_H1; else p->resyncs++;
        return false;
    case S_H1:
        if (b == HDR1) { p->state = S_H2; return false; }
        break;
    case S_H2:
        if (b == HDR2) { p->state = S_LEN; return false; }
        break;
    case S_LEN:
        /* LEN counts itself, CMD and CRC: 3 is an empty DATA field. */
        if (b < 3 || b > 3 + BK_FRAME_MAX_DATA) break;
        p->len = b;
        p->idx = 0;
        p->state = S_BODY;
        return false;
    case S_BODY:
        p->body[p->idx++] = b;
        if (p->idx >= p->len - 1) p->state = S_F0;   /* CMD + DATA + CRC read */
        return false;
    case S_F0:
        if (b == FTR0) { p->state = S_F1; return false; }
        break;
    case S_F1:
        if (b == FTR1) { p->state = S_F2; return false; }
        break;
    case S_F2: {
        p->state = S_H0;
        if (b != FTR2) { p->resyncs++; return false; }
        uint8_t crc = p->len;
        for (uint8_t i = 0; i + 1 < p->idx; i++) crc = (uint8_t)(crc + p->body[i]);
        if (crc != p->body[p->idx - 1]) { p->crc_errors++; return false; }
        out->cmd = p->body[0];
        out->n = (uint8_t)(p->idx - 2);
        memcpy(out->data, &p->body[1], out->n);
        p->frames++;
        return true;
    }
    }
    /* Anything that broke the pattern: start over, but a stray 0x52 may be
     * the first byte of the real header. */
    p->resyncs++;
    p->state = (b == HDR0) ? S_H1 : S_H0;
    return false;
}

/* ---- state -------------------------------------------------------------- */

void bk_state_init(bk_state_t *s)
{
    memset(s, 0, sizeof(*s));
    s->volume = -1;
    s->last_status = -1;
}

/* Printable text only: names and numbers come from a phone. */
static void copy_text(char *dst, size_t dst_len, const uint8_t *src, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < dst_len; i++) {
        uint8_t c = src[i];
        if (c == 0) break;
        dst[o++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    dst[o] = '\0';
}

static bool set_call(bk_state_t *s, bk_call_t c)
{
    if (s->call == c) return false;
    s->call = c;
    if (c == BK_CALL_IDLE) s->caller_id[0] = '\0';
    return true;
}

static unsigned set_connected(bk_state_t *s, bool up)
{
    unsigned chg = 0;
    if (s->connected != up) { s->connected = up; chg |= BK_CHG_LINK; }
    if (!up) {
        if (s->phone_name[0]) { s->phone_name[0] = '\0'; chg |= BK_CHG_INFO; }
        if (set_call(s, BK_CALL_IDLE)) chg |= BK_CHG_CALL;
    }
    return chg;
}

static void format_addr(char *dst, size_t dst_len, const uint8_t *d, size_t n)
{
    if (n == 6) {
        snprintf(dst, dst_len, "%02X:%02X:%02X:%02X:%02X:%02X", d[0], d[1], d[2], d[3], d[4], d[5]);
    } else {
        copy_text(dst, dst_len, d, n);   /* some firmware sends it as text */
    }
}

unsigned bk_state_apply(bk_state_t *s, const bk_frame_t *f)
{
    unsigned chg = 0;
    switch (f->cmd) {
    case BK_MID_ACK: {
        if (f->n < 1) break;
        uint8_t st = f->data[0];
        s->last_status = st;
        switch (st) {
        case BK_ST_ON:
            bk_state_init(s);
            s->booted = true;
            s->last_status = st;
            chg |= BK_CHG_BOOT | BK_CHG_LINK | BK_CHG_CALL | BK_CHG_INFO;
            break;
        case BK_ST_II:
        case BK_ST_C1:
            chg |= set_connected(s, true);
            break;
        case BK_ST_M1:                       /* linked, and no call */
            chg |= set_connected(s, true);
            if (set_call(s, BK_CALL_IDLE)) chg |= BK_CHG_CALL;
            break;
        case BK_ST_C0:
        case BK_ST_M0:
        case BK_ST_IA:
            chg |= set_connected(s, false);
            break;
        case BK_ST_IC:
            s->connected = true;
            if (set_call(s, BK_CALL_INCOMING)) chg |= BK_CHG_CALL;
            break;
        case BK_ST_M2:
        case BK_ST_M3:
        case BK_ST_ID:
            s->connected = true;
            if (set_call(s, BK_CALL_OUTGOING)) chg |= BK_CHG_CALL;
            break;
        case BK_ST_M4:
            s->connected = true;
            if (set_call(s, BK_CALL_ACTIVE)) chg |= BK_CHG_CALL;
            break;
        default:
            break;
        }
        break;
    }
    case BK_MID_IR: {
        char num[BK_NUMBER_MAX];
        copy_text(num, sizeof(num), f->data, f->n);
        if (strcmp(num, s->caller_id) != 0) {
            memcpy(s->caller_id, num, sizeof(num));
            chg |= BK_CHG_CALL;
        }
        break;
    }
    case BK_MID_VOL:
        if (f->n >= 1) { s->volume = f->data[0]; chg |= BK_CHG_INFO; }
        break;
    case BK_MID_ADDR:
        format_addr(s->local_addr, sizeof(s->local_addr), f->data, f->n);
        chg |= BK_CHG_INFO;
        break;
    case BK_MID_NAME:
        copy_text(s->local_name, sizeof(s->local_name), f->data, f->n);
        chg |= BK_CHG_INFO;
        break;
    case BK_MID_PHONENAME:
        copy_text(s->phone_name, sizeof(s->phone_name), f->data, f->n);
        chg |= BK_CHG_INFO;
        break;
    case BK_MID_VERSION:
        copy_text(s->version, sizeof(s->version), f->data, f->n);
        chg |= BK_CHG_INFO;
        break;
    default:
        break;
    }
    return chg;
}

/* ---- description (bring-up logging) ------------------------------------- */

const char *bk_status_name(uint8_t st)
{
    switch (st) {
    case BK_ST_OK:     return "OK (command accepted)";
    case BK_ST_MA:     return "MA time out";
    case BK_ST_MB:     return "MB play";
    case BK_ST_ON:     return "ON booted";
    case BK_ST_II:     return "II link established";
    case BK_ST_C0:     return "C0 ACL not connected";
    case BK_ST_C1:     return "C1 ACL connected";
    case BK_ST_M0:     return "M0 not connected";
    case BK_ST_M1:     return "M1 HFP connected, no call";
    case BK_ST_M2:     return "M2 calling out";
    case BK_ST_M3:     return "M3 calling out";
    case BK_ST_M4:     return "M4 in a call";
    case BK_ST_IC:     return "IC incoming call";
    case BK_ST_ID:     return "ID calling out";
    case BK_ST_AUX:    return "AUX transmit mode";
    case BK_ST_BT:     return "BT receive mode";
    case BK_ST_END:    return "END search finished";
    case BK_ST_WC:     return "WC transmit pairing ok";
    case BK_ST_WD:     return "WD transmit disconnect";
    case BK_ST_IA:     return "IA disconnected";
    case BK_ST_IDLE:   return "IDLE";
    case BK_ST_MUTE:   return "MUTE";
    case BK_ST_UNMUTE: return "UNMUTE";
    default:           return "unknown";
    }
}

void bk_describe(const bk_frame_t *f, char *out, size_t out_len)
{
    char text[BK_FRAME_MAX_DATA + 1];
    copy_text(text, sizeof(text), f->data, f->n);
    switch (f->cmd) {
    case BK_MID_ACK:
        if (f->n >= 1) snprintf(out, out_len, "status 0x%02X %s", f->data[0], bk_status_name(f->data[0]));
        else snprintf(out, out_len, "status (empty)");
        return;
    case BK_MID_PR:        snprintf(out, out_len, "outgoing number '%s'", text); return;
    case BK_MID_IR:        snprintf(out, out_len, "caller number '%s'", text); return;
    case BK_MID_VOL:       snprintf(out, out_len, "volume %d", f->n ? f->data[0] : -1); return;
    case BK_MID_ADDR: {
        char a[BK_ADDR_MAX];
        format_addr(a, sizeof(a), f->data, f->n);
        snprintf(out, out_len, "own address %s", a);
        return;
    }
    case BK_MID_NAME:      snprintf(out, out_len, "own name '%s'", text); return;
    case BK_MID_PHONENAME: snprintf(out, out_len, "phone name '%s'", text); return;
    case BK_MID_VERSION:   snprintf(out, out_len, "version '%s'", text); return;
    default:
        snprintf(out, out_len, "cmd 0x%02X, %u data byte%s", f->cmd, f->n, f->n == 1 ? "" : "s");
        return;
    }
}
