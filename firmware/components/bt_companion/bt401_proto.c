/* bt401_proto.c — see bt401_proto.h. Pure C, no ESP-IDF. */
#include "bt401_proto.h"

#include <stdio.h>
#include <string.h>

/* strlcpy isn't in every libc the host tests build on. */
static void copy_str(char *dst, const char *src, size_t n)
{
    if (!n) return;
    size_t l = strlen(src);
    if (l >= n) l = n - 1;
    memcpy(dst, src, l);
    dst[l] = 0;
}

static size_t put_line(char *out, size_t out_max, const char *a, const char *b)
{
    size_t la = strlen(a), lb = b ? strlen(b) : 0;
    size_t need = 3 + la + lb + 2;   /* "AT+" + cmd + param + CRLF */
    if (!out || need + 1 > out_max) return 0;
    memcpy(out, "AT+", 3);
    memcpy(out + 3, a, la);
    if (lb) memcpy(out + 3 + la, b, lb);
    memcpy(out + 3 + la + lb, "\r\n", 2);
    out[need] = 0;
    return need;
}

size_t bt401_build_cmd(const char *cmd, const char *param, char *out, size_t out_max)
{
    if (!cmd || !*cmd) return 0;
    return put_line(out, out_max, cmd, param);
}

size_t bt401_build_dial(const char *number, char *out, size_t out_max)
{
    if (!number || !*number) return 0;
    size_t n = strlen(number);
    if (n > BT401_DIAL_MAX) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = number[i];
        if (!((c >= '0' && c <= '9') || c == '*' || c == '#' || c == '+')) return 0;
    }
    return put_line(out, out_max, "BT", number);
}

/* ---- decoding ------------------------------------------------------------ */

static int parse_int(const char *s)
{
    int v = 0, digits = 0;
    while (*s >= '0' && *s <= '9' && digits < 6) { v = v * 10 + (*s - '0'); s++; digits++; }
    return digits ? v : -1;
}

/* "9EE884AB8BCC" -> "9E:E8:84:AB:8B:CC" (anything else is passed through). */
static void format_addr(const char *hex, char *out, size_t out_max)
{
    size_t n = strlen(hex);
    if (n == 12 && out_max >= 18) {
        size_t o = 0;
        for (size_t i = 0; i < 12; i += 2) {
            if (i) out[o++] = ':';
            out[o++] = hex[i];
            out[o++] = hex[i + 1];
        }
        out[o] = 0;
        return;
    }
    copy_str(out, hex, out_max);
}

void bt401_decode_line(const char *line, bt401_msg_t *out)
{
    memset(out, 0, sizeof(*out));
    out->num = -1;
    if (strcmp(line, "OK") == 0) { out->kind = BT401_MSG_OK; return; }
    if (strlen(line) < 3 || line[2] != '+') return;          /* kind stays NONE */
    out->code[0] = line[0];
    out->code[1] = line[1];
    const char *v = line + 3;
    copy_str(out->text, v, sizeof(out->text));
    out->num = parse_int(v);

    if (!strcmp(out->code, "ER"))      out->kind = BT401_MSG_ERROR;
    else if (!strcmp(out->code, "TS")) out->kind = out->num >= 0 ? BT401_MSG_STATUS : BT401_MSG_OTHER;
    else if (!strcmp(out->code, "TT")) out->kind = BT401_MSG_NUMBER;
    else if (!strcmp(out->code, "TD")) out->kind = BT401_MSG_NAME;
    else if (!strcmp(out->code, "TA")) {
        out->kind = BT401_MSG_ADDR;
        format_addr(v, out->text, sizeof(out->text));
    }
    else if (!strcmp(out->code, "QA")) out->kind = BT401_MSG_VOLUME;
    else if (!strcmp(out->code, "QM")) out->kind = BT401_MSG_MODE;
    else out->kind = BT401_MSG_OTHER;
}

void bt401_parser_init(bt401_parser_t *p)
{
    memset(p, 0, sizeof(*p));
}

bool bt401_parser_feed(bt401_parser_t *p, uint8_t byte, bt401_msg_t *out)
{
    p->bytes++;
    if (byte == '\r') return false;
    if (byte != '\n') {
        if (p->len < BT401_LINE_MAX - 1) p->buf[p->len++] = (char)byte;
        else p->overflow = true;
        return false;
    }
    bool dropped = p->overflow;
    size_t n = p->len;
    p->buf[n] = 0;
    p->len = 0;
    p->overflow = false;
    if (dropped) { p->overflows++; return false; }
    if (n == 0) return false;
    p->lines++;
    bt401_decode_line(p->buf, out);
    return true;
}

/* ---- state --------------------------------------------------------------- */

void bt401_state_init(bt401_state_t *s)
{
    memset(s, 0, sizeof(*s));
    s->ts = -1;
}

unsigned bt401_state_apply(bt401_state_t *s, const bt401_msg_t *m)
{
    unsigned chg = 0;
    switch (m->kind) {
    case BT401_MSG_STATUS: {
        int ts = m->num;
        bool connected = ts >= BT401_TS_IDLE;
        bool music = ts == BT401_TS_MUSIC;
        bt401_call_t call = BT401_CALL_IDLE;
        if (ts == BT401_TS_RINGING) call = s->dialing ? BT401_CALL_OUTGOING : BT401_CALL_INCOMING;
        else if (ts == BT401_TS_IN_CALL) call = BT401_CALL_ACTIVE;
        s->ts = ts;
        if (connected != s->connected) { s->connected = connected; chg |= BT401_CHG_LINK; }
        if (music != s->music) { s->music = music; chg |= BT401_CHG_MUSIC; }
        if (call != s->call) { s->call = call; chg |= BT401_CHG_CALL; }
        if (call == BT401_CALL_IDLE) {
            s->dialing = false;
            if (s->caller_id[0]) { s->caller_id[0] = 0; chg |= BT401_CHG_CALLER; }
        }
        break;
    }
    case BT401_MSG_NUMBER:
        if (strncmp(s->caller_id, m->text, sizeof(s->caller_id) - 1) != 0) {
            copy_str(s->caller_id, m->text, sizeof(s->caller_id));
            chg |= BT401_CHG_CALLER;
        }
        break;
    case BT401_MSG_NAME:
        copy_str(s->name, m->text, sizeof(s->name));
        chg |= BT401_CHG_INFO;
        break;
    case BT401_MSG_ADDR:
        copy_str(s->addr, m->text, sizeof(s->addr));
        chg |= BT401_CHG_INFO;
        break;
    case BT401_MSG_ERROR:
        s->errors++;
        if (s->call == BT401_CALL_IDLE) s->dialing = false;   /* the dial command was refused */
        break;
    default:
        break;
    }
    return chg;
}

void bt401_describe(const bt401_msg_t *m, char *out, size_t out_max)
{
    switch (m->kind) {
    case BT401_MSG_OK:     snprintf(out, out_max, "OK"); break;
    case BT401_MSG_ERROR:  snprintf(out, out_max, "error %d", m->num); break;
    case BT401_MSG_STATUS: {
        static const char *const names[] = { "waiting for a phone", "connected, idle", "playing music",
                                             "ringing", "in a call" };
        snprintf(out, out_max, "status %d (%s)", m->num, m->num >= 0 && m->num <= 4 ? names[m->num] : "?");
        break;
    }
    case BT401_MSG_NUMBER: snprintf(out, out_max, "number %s", m->text); break;
    case BT401_MSG_NAME:   snprintf(out, out_max, "own name %s", m->text); break;
    case BT401_MSG_ADDR:   snprintf(out, out_max, "own address %s", m->text); break;
    case BT401_MSG_VOLUME: snprintf(out, out_max, "volume %d", m->num); break;
    case BT401_MSG_MODE:   snprintf(out, out_max, "mode %d", m->num); break;
    case BT401_MSG_OTHER:  snprintf(out, out_max, "%s+%s", m->code, m->text); break;
    default:               snprintf(out, out_max, "(nothing)"); break;
    }
}
