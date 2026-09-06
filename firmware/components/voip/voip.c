/*
 * voip.c — minimal embedded SIP/RTP user agent (the real backend).
 *
 * A hand-rolled SIP UA aimed squarely at "register to a LAN Asterisk and do
 * G.711 calls" -- no TLS/SRTP, no proxies, no forking. One UDP socket on
 * :5060 handles signaling for both lines; media is RTP/PCMU via rtp.c bridged
 * to the audio component. Digest auth (MD5, qop=auth) is supported.
 *
 * Scope on purpose (fallback is ESP-ADF esp_sip if this proves too limited):
 *   REGISTER (+refresh), outbound INVITE, inbound INVITE, ACK, BYE, CANCEL,
 *   200/486/487/603 responses, single audio stream per line.
 *
 * Config lives in NVS ("voip" namespace): server + per-line user/password.
 */
#include "voip.h"
#include "rtp.h"
#include "audio.h"
#include "wifi.h"
#include "board.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "mdns.h"
#include "mbedtls/md5.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "voip";

#define SIP_PORT        5060
#define NLINES          2
#define VOIP_NVS_NS     "voip"
#define REG_EXPIRES     120        /* seconds; refresh at half */
#define REG_RETRY_S     5          /* safety-net retry if a REGISTER round trip goes unanswered */
#define REG_FAIL_BACKOFF_S 120     /* retry interval after a DEFINITIVE rejection (401 even with
                                    * correct-looking auth) -- REG_RETRY_S is for silence/packet
                                    * loss, not this: retrying unchanged bad credentials every 5s
                                    * forever just hammers the server and, worse, was observed to
                                    * starve this device's own WiFi/UDP stack badly enough to hang
                                    * the web UI. */
#define REG_AUTH_RETRY_SAFETY_S 20 /* safety-net for the authenticated retry specifically --
                                    * longer than REG_RETRY_S so a slow (not lost) 401/200 on a
                                    * lossy link doesn't get raced by the periodic safety-net
                                    * resetting reg_authed_once before that response is even
                                    * processed, which was silently defeating REG_FAIL_BACKOFF_S
                                    * above (see send_register()'s doc comment). */
#define MSG_MAX         1600

typedef enum { CALL_IDLE = 0, CALL_OUTGOING, CALL_INCOMING, CALL_ACTIVE } call_state_t;

typedef struct {
    /* account / registration */
    char user[32];
    char pass[48];
    bool enabled;
    bool registered;
    uint32_t reg_cseq;
    int64_t  reg_refresh_at_us;   /* 0 = not scheduled */
    bool     reg_authed_once;     /* avoid infinite 401 loops */

    /* current dialog */
    call_state_t state;
    char     call_id[80];
    char     from_tag[24];
    char     to_tag[80];          /* peer's tag */
    uint32_t cseq;
    char     branch[40];          /* INVITE transaction branch (for ACK/CANCEL) */
    char     remote_target[128];  /* peer Contact URI for in-dialog requests */
    char     remote_uri[96];      /* the URI we INVITEd / the peer's From URI */
    char     remote_name[40];     /* caller-ID display name, "" if none sent */
    char     peer_rtp_ip[16];
    uint16_t peer_rtp_port;
    bool     call_authed_once;
    char     dial_uri[96];        /* for resending INVITE after a 401 */

    /* where to send responses for the most recent inbound request in-dialog */
    struct sockaddr_in reply_addr;
    /* saved inbound-request headers, to build responses */
    char     in_via[256];
    char     in_from[160];
    char     in_to[224];          /* with our tag appended */
    uint32_t in_cseq;
} line_t;

static line_t         s_l[NLINES];
static voip_event_cb  s_cb;
static void          *s_ctx;
static int            s_sock = -1;
static char           s_server[64];
static char           s_server_ip[16];
static struct sockaddr_in s_server_addr;
static char           s_myip[16];
static TaskHandle_t   s_task;
/* Shared "setup" secret, per board -- see voip_setup_password()'s doc
 * comment in voip.h. Stored alongside the rest of the VoIP config since it
 * started life there (as the auto-provisioner's password); kept there since
 * there's no better home for a lone per-board secret. */
static char           s_setup_pass[80];

/* ---------- small helpers ---------- */

static void emit(int line, voip_event_type_t type, const char *peer)
{
    if (!s_cb) return;
    voip_event_t ev = { .line = line, .type = type };
    if (peer) strncpy(ev.peer, peer, sizeof(ev.peer) - 1);
    s_cb(&ev, s_ctx);
}

static void rand_hex(char *out, size_t n)
{
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n - 1; i++) out[i] = hx[esp_random() & 0xF];
    out[n - 1] = '\0';
}

static void md5_hex(const char *in, char *out33)
{
    uint8_t d[16];
    mbedtls_md5((const unsigned char *)in, strlen(in), d);
    for (int i = 0; i < 16; i++) sprintf(out33 + i * 2, "%02x", d[i]);
    out33[32] = '\0';
}

/* Copy a header's value (first match, full or compact name) into out, up to
 * CRLF. Case-insensitive on the header name. */
static bool get_header(const char *msg, const char *name, const char *cname,
                       char *out, size_t outsz)
{
    size_t nl = strlen(name), cl = cname ? strlen(cname) : 0;
    for (const char *p = msg; *p; ) {
        const char *eol = strstr(p, "\r\n");
        if (!eol) break;
        bool match = (strncasecmp(p, name, nl) == 0 && p[nl] == ':');
        size_t skip = nl;
        if (!match && cname && strncasecmp(p, cname, cl) == 0 && p[cl] == ':') { match = true; skip = cl; }
        if (match) {
            const char *v = p + skip + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t len = (size_t)(eol - v);
            if (len >= outsz) len = outsz - 1;
            memcpy(out, v, len);
            out[len] = '\0';
            return true;
        }
        p = eol + 2;
        if (p[0] == '\r' && p[1] == '\n') break; /* end of headers */
    }
    return false;
}

/* Extract a ";tag=..." value from a From/To header value. */
static void get_tag(const char *hdrval, char *out, size_t outsz)
{
    out[0] = '\0';
    const char *t = strstr(hdrval, "tag=");
    if (!t) return;
    t += 4;
    size_t i = 0;
    while (t[i] && t[i] != ';' && t[i] != '>' && t[i] != ' ' && i < outsz - 1) { out[i] = t[i]; i++; }
    out[i] = '\0';
}

/* Pull a quoted or token auth param (realm, nonce, qop) from an
 * Authenticate header value. */
static void auth_param(const char *h, const char *key, char *out, size_t outsz)
{
    out[0] = '\0';
    char pat[24];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = strstr(h, pat);
    if (!p) return;
    p += strlen(pat);
    if (*p == '"') {
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i < outsz - 1) out[i++] = *p++;
        out[i] = '\0';
    } else {
        size_t i = 0;
        while (*p && *p != ',' && *p != '\r' && *p != ' ' && i < outsz - 1) out[i++] = *p++;
        out[i] = '\0';
    }
}

/* Angle-bracketed URI from a Contact/To value: <sip:...> -> sip:... */
/* Caller-ID name from a From/To header value: the display name that precedes
 * the <sip:...> URI, quoted ("Max Phone" <sip:...>) or bare (Max <sip:...>).
 * Yields "" when there's no display name at all -- plenty of UAs send a bare
 * URI, and this firmware's own outbound INVITEs did until recently -- so
 * callers must treat "no name" as normal and fall back to the number. */
static void get_display_name(const char *hdrval, char *out, size_t outsz)
{
    out[0] = '\0';
    while (*hdrval == ' ' || *hdrval == '\t') hdrval++;

    if (*hdrval == '"') {
        hdrval++;
        size_t i = 0;
        while (*hdrval && *hdrval != '"' && i < outsz - 1) {
            if (*hdrval == '\\' && hdrval[1]) hdrval++; /* quoted-pair escape */
            out[i++] = *hdrval++;
        }
        out[i] = '\0';
        return;
    }

    /* Unquoted: everything before '<', trailing whitespace trimmed. No '<'
     * at all means the header is a bare URI (no display name) -- not a name,
     * so leave the result empty rather than returning "sip:1066@host". */
    const char *lt = strchr(hdrval, '<');
    if (!lt) return;
    size_t n = (size_t)(lt - hdrval);
    while (n > 0 && (hdrval[n - 1] == ' ' || hdrval[n - 1] == '\t')) n--;
    if (n > outsz - 1) n = outsz - 1;
    memcpy(out, hdrval, n);
    out[n] = '\0';
}

/* User part of a SIP URI: "sip:1066@host:port" -> "1066". Falls back to
 * copying the whole thing when there's no user@ to extract. */
static void uri_user(const char *uri, char *out, size_t outsz)
{
    const char *s = strchr(uri, ':');
    s = s ? s + 1 : uri;
    size_t i = 0;
    while (s[i] && s[i] != '@' && i < outsz - 1) { out[i] = s[i]; i++; }
    out[i] = '\0';
}

static void get_uri(const char *hdrval, char *out, size_t outsz)
{
    const char *a = strchr(hdrval, '<');
    if (a) {
        a++;
        size_t i = 0;
        while (*a && *a != '>' && i < outsz - 1) out[i++] = *a++;
        out[i] = '\0';
    } else {
        strncpy(out, hdrval, outsz - 1);
        out[outsz - 1] = '\0';
        char *sp = strpbrk(out, " ;");
        if (sp) *sp = '\0';
    }
}

/* Remote RTP endpoint from an SDP body (c=IN IP4 <ip>, m=audio <port> ...). */
static bool parse_sdp(const char *msg, char *ip, size_t ipsz, uint16_t *port)
{
    const char *body = strstr(msg, "\r\n\r\n");
    if (!body) return false;
    body += 4;
    const char *c = strstr(body, "c=IN IP4 ");
    const char *m = strstr(body, "m=audio ");
    if (!c || !m) return false;
    c += 9;
    size_t i = 0;
    while (c[i] && c[i] != '\r' && c[i] != '\n' && i < ipsz - 1) { ip[i] = c[i]; i++; }
    ip[i] = '\0';
    *port = (uint16_t)atoi(m + 8);
    return (*port != 0 && ip[0]);
}

/* ---------- digest auth ---------- */

static void build_authorization(line_t *L, const char *method, const char *uri,
                                const char *hdrval, char *out, size_t outsz,
                                const char *hdrname)
{
    char realm[64], nonce[128], qop[16];
    auth_param(hdrval, "realm", realm, sizeof(realm));
    auth_param(hdrval, "nonce", nonce, sizeof(nonce));
    auth_param(hdrval, "qop", qop, sizeof(qop));

    char ha1[33], ha2[33], resp[33], tmp[400];
    snprintf(tmp, sizeof(tmp), "%s:%s:%s", L->user, realm, L->pass);
    md5_hex(tmp, ha1);
    snprintf(tmp, sizeof(tmp), "%s:%s", method, uri);
    md5_hex(tmp, ha2);

    if (qop[0]) { /* qop=auth: MD5(HA1:nonce:nc:cnonce:qop:HA2) */
        char cnonce[17];
        rand_hex(cnonce, sizeof(cnonce));
        snprintf(tmp, sizeof(tmp), "%s:%s:00000001:%s:auth:%s", ha1, nonce, cnonce, ha2);
        md5_hex(tmp, resp);
        snprintf(out, outsz,
            "%s: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", "
            "response=\"%s\", qop=auth, nc=00000001, cnonce=\"%s\", algorithm=MD5\r\n",
            hdrname, L->user, realm, nonce, uri, resp, cnonce);
    } else {      /* plain: MD5(HA1:nonce:HA2) */
        snprintf(tmp, sizeof(tmp), "%s:%s:%s", ha1, nonce, ha2);
        md5_hex(tmp, resp);
        snprintf(out, outsz,
            "%s: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", "
            "response=\"%s\", algorithm=MD5\r\n",
            hdrname, L->user, realm, nonce, uri, resp);
    }
}

/* ---------- outbound signaling ---------- */

static void udp_send(const char *msg, const struct sockaddr_in *to)
{
    sendto(s_sock, msg, strlen(msg), 0, (const struct sockaddr *)to, sizeof(*to));
}

static void send_register(int line, const char *auth_hdr, int safety_net_s)
{
    line_t *L = &s_l[line];
    char branch[40]; rand_hex(branch, sizeof(branch));
    L->reg_cseq++;
    char buf[MSG_MAX];
    int n = snprintf(buf, sizeof(buf),
        "REGISTER sip:%s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=z9hG4bK%s;rport\r\n"
        "Max-Forwards: 70\r\n"
        "From: <sip:%s@%s>;tag=%s\r\n"
        "To: <sip:%s@%s>\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %u REGISTER\r\n"
        "Contact: <sip:%s@%s:%d>\r\n"
        "Expires: %d\r\n"
        "User-Agent: RotaryPhone-ESP32\r\n"
        "%s"
        "Content-Length: 0\r\n\r\n",
        s_server, s_myip, SIP_PORT, branch,
        L->user, s_server, L->from_tag,
        L->user, s_server,
        L->call_id, (unsigned)L->reg_cseq,
        L->user, s_myip, SIP_PORT,
        REG_EXPIRES,
        auth_hdr ? auth_hdr : "");
    if (n > 0 && n < (int)sizeof(buf)) udp_send(buf, &s_server_addr);
    /* Safety-net retry: REGISTER's 401-challenge/authenticated-retry dance
     * is two UDP round trips with no reliability of its own -- if any leg
     * (this send, the 401, the retry, or the final 200 OK) gets lost, there
     * was previously no fallback at all: reg_refresh_at_us only ever got
     * (re)armed by handle_response()'s 200 OK case, so a single dropped
     * packet silently stopped every future refresh forever, while
     * `registered` stayed true locally (never corrected) until reboot --
     * exactly what happened to a board that had been up 35 minutes with no
     * indication anything was wrong. Arming a retry on every send,
     * unconditionally, means a lost packet self-heals instead of needing a
     * power cycle; an actual 200 OK below overrides this with the real
     * REG_EXPIRES/2 interval.
     *
     * `safety_net_s` differs by call site on purpose: the bare (challenge)
     * send uses the short REG_RETRY_S, but the authenticated retry (sent
     * from handle_response()'s 401 handler) needs longer -- on a lossy link,
     * REG_RETRY_S can elapse before that retry's OWN response comes back,
     * and this same periodic path is what resets reg_authed_once to false
     * to try again. If that happens before the retry's 401/200 is even
     * processed, reg_authed_once never gets to be true when a second 401
     * actually arrives, so handle_response()'s "definitive rejection, back
     * off" branch can never fire -- seen live: a bad password produced a
     * 401 every ~5-9s forever instead of the intended one-shot-then-120s-
     * backoff, and that storm alone was enough to starve the web UI again. */
    L->reg_refresh_at_us = esp_timer_get_time() + (int64_t)safety_net_s * 1000000;
}

static void send_invite(int line, const char *auth_hdr)
{
    line_t *L = &s_l[line];
    L->cseq++;
    char sdp[400];
    int slen = snprintf(sdp, sizeof(sdp),
        "v=0\r\no=- %u %u IN IP4 %s\r\ns=phone\r\nc=IN IP4 %s\r\nt=0 0\r\n"
        "m=audio %u RTP/AVP 0 101\r\na=rtpmap:0 PCMU/8000\r\n"
        "a=rtpmap:101 telephone-event/8000\r\na=fmtp:101 0-16\r\na=ptime:20\r\na=sendrecv\r\n",
        (unsigned)esp_random(), (unsigned)esp_random(), s_myip, s_myip, rtp_local_port(line));
    /* Outbound caller ID: the board's own name ("MaxPhone"), as the From
     * display name -- this is what shows up on the callee's screen and in
     * the PBX's CDR. Quoted per RFC 3261, with any embedded quote/backslash
     * dropped rather than escaped, since a board name is user-settable and
     * an unescaped one would corrupt the header. */
    char bname[32], cid[40] = "";
    board_name_get(bname, sizeof(bname));
    size_t ci = 0;
    for (size_t i = 0; bname[i] && ci < sizeof(cid) - 4; i++) {
        if (bname[i] == '"' || bname[i] == '\\') continue;
        cid[ci++] = bname[i];
    }
    cid[ci] = '\0';

    char buf[MSG_MAX];
    int n = snprintf(buf, sizeof(buf),
        "INVITE %s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=z9hG4bK%s;rport\r\n"
        "Max-Forwards: 70\r\n"
        "From: \"%s\" <sip:%s@%s>;tag=%s\r\n"
        "To: <%s>\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %u INVITE\r\n"
        "Contact: <sip:%s@%s:%d>\r\n"
        "%s"
        "Content-Type: application/sdp\r\n"
        "User-Agent: RotaryPhone-ESP32\r\n"
        "Content-Length: %d\r\n\r\n%s",
        L->dial_uri,
        s_myip, SIP_PORT, L->branch,
        cid, L->user, s_server, L->from_tag,
        L->dial_uri,
        L->call_id, (unsigned)L->cseq,
        L->user, s_myip, SIP_PORT,
        auth_hdr ? auth_hdr : "",
        slen, sdp);
    if (n > 0 && n < (int)sizeof(buf)) udp_send(buf, &s_server_addr);
}

/* Cancels our own outstanding INVITE (call still ringing, not yet
 * answered). Per RFC 3261 SS9.1, unlike BYE (a new transaction), CANCEL
 * MUST reuse the exact same Via branch and CSeq number as the INVITE it's
 * cancelling -- L->branch/L->cseq are still exactly what send_invite() set
 * them to, since nothing touches them again before a final response. */
static void send_cancel(int line)
{
    line_t *L = &s_l[line];
    char buf[MSG_MAX];
    int n = snprintf(buf, sizeof(buf),
        "CANCEL %s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=z9hG4bK%s;rport\r\n"
        "Max-Forwards: 70\r\n"
        "From: <sip:%s@%s>;tag=%s\r\n"
        "To: <%s>%s%s\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %u CANCEL\r\n"
        "Content-Length: 0\r\n\r\n",
        L->dial_uri,
        s_myip, SIP_PORT, L->branch,
        L->user, s_server, L->from_tag,
        L->dial_uri, L->to_tag[0] ? ";tag=" : "", L->to_tag,
        L->call_id, (unsigned)L->cseq);
    if (n > 0 && n < (int)sizeof(buf)) udp_send(buf, &s_server_addr);
}

/* ACK. For a 2xx it's a new transaction (fresh branch) to the peer's target;
 * for a non-2xx final it must reuse the INVITE's Via branch. */
static void send_ack(int line, bool twoxx)
{
    line_t *L = &s_l[line];
    char newbranch[40];
    const char *branch = L->branch;
    if (twoxx) { rand_hex(newbranch, sizeof(newbranch)); branch = newbranch; }
    char buf[MSG_MAX];
    int n = snprintf(buf, sizeof(buf),
        "ACK %s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=z9hG4bK%s;rport\r\n"
        "Max-Forwards: 70\r\n"
        "From: <sip:%s@%s>;tag=%s\r\n"
        "To: <%s>;tag=%s\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %u ACK\r\n"
        "Content-Length: 0\r\n\r\n",
        L->remote_target[0] ? L->remote_target : L->dial_uri,
        s_myip, SIP_PORT, branch,
        L->user, s_server, L->from_tag,
        L->remote_uri, L->to_tag,
        L->call_id, (unsigned)L->cseq);
    if (n > 0 && n < (int)sizeof(buf)) udp_send(buf, &s_server_addr);
}

static void send_bye(int line)
{
    line_t *L = &s_l[line];
    char branch[40]; rand_hex(branch, sizeof(branch));
    L->cseq++;
    char buf[MSG_MAX];
    int n = snprintf(buf, sizeof(buf),
        "BYE %s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=z9hG4bK%s;rport\r\n"
        "Max-Forwards: 70\r\n"
        "From: <sip:%s@%s>;tag=%s\r\n"
        "To: <%s>;tag=%s\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %u BYE\r\n"
        "Content-Length: 0\r\n\r\n",
        L->remote_target[0] ? L->remote_target : L->remote_uri,
        s_myip, SIP_PORT, branch,
        L->user, s_server, L->from_tag,
        L->remote_uri, L->to_tag,
        L->call_id, (unsigned)L->cseq);
    if (n > 0 && n < (int)sizeof(buf)) udp_send(buf, &s_server_addr);
}

/* Response to an inbound request, echoing its Via/From/To/Call-ID/CSeq. */
static void send_response(line_t *L, int code, const char *reason, const char *method,
                          bool with_sdp, int line)
{
    char sdp[400] = "";
    int slen = 0;
    char extra[200] = "";
    if (with_sdp) {
        slen = snprintf(sdp, sizeof(sdp),
            "v=0\r\no=- %u %u IN IP4 %s\r\ns=phone\r\nc=IN IP4 %s\r\nt=0 0\r\n"
            "m=audio %u RTP/AVP 0 101\r\na=rtpmap:0 PCMU/8000\r\n"
            "a=rtpmap:101 telephone-event/8000\r\na=fmtp:101 0-16\r\na=ptime:20\r\na=sendrecv\r\n",
            (unsigned)esp_random(), (unsigned)esp_random(), s_myip, s_myip, rtp_local_port(line));
        snprintf(extra, sizeof(extra),
            "Contact: <sip:%s@%s:%d>\r\nContent-Type: application/sdp\r\n", L->user, s_myip, SIP_PORT);
    }
    char buf[MSG_MAX];
    int n = snprintf(buf, sizeof(buf),
        "SIP/2.0 %d %s\r\n"
        "Via: %s\r\n"
        "From: %s\r\n"
        "To: %s\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %u %s\r\n"
        "%s"
        "Content-Length: %d\r\n\r\n%s",
        code, reason, L->in_via, L->in_from, L->in_to, L->call_id, (unsigned)L->in_cseq, method,
        extra, slen, sdp);
    if (n > 0 && n < (int)sizeof(buf)) udp_send(buf, &L->reply_addr);
}

/* ---------- inbound handling ---------- */

static int line_for_user(const char *user)
{
    for (int i = 0; i < NLINES; i++)
        if (s_l[i].enabled && strcmp(s_l[i].user, user) == 0) return i;
    return -1;
}

static int line_for_callid(const char *callid)
{
    for (int i = 0; i < NLINES; i++)
        if (s_l[i].state != CALL_IDLE && strcmp(s_l[i].call_id, callid) == 0) return i;
    return -1;
}

static void reset_call(line_t *L)
{
    L->state = CALL_IDLE;
    L->to_tag[0] = L->remote_target[0] = L->remote_uri[0] = L->remote_name[0] = 0;
    L->peer_rtp_ip[0] = 0; L->peer_rtp_port = 0;
    L->call_authed_once = false;
}

static void handle_response(const char *msg, const struct sockaddr_in *src)
{
    int code = atoi(msg + 8); /* after "SIP/2.0 " */
    char callid[80], cseq[64];
    if (!get_header(msg, "Call-ID", "i", callid, sizeof(callid))) return;
    get_header(msg, "CSeq", NULL, cseq, sizeof(cseq));
    bool is_reg = strstr(cseq, "REGISTER") != NULL;
    bool is_inv = strstr(cseq, "INVITE") != NULL;

    int line = -1;
    for (int i = 0; i < NLINES; i++) if (strcmp(s_l[i].call_id, callid) == 0) { line = i; break; }
    if (line < 0) return;
    line_t *L = &s_l[line];

    if (is_reg) {
        if (code == 401 || code == 407) {
            if (L->reg_authed_once) {
                L->registered = false;
                /* Definitive rejection, not silence -- retrying the same bad
                 * credentials via the REG_RETRY_S safety net every 5s would
                 * just repeat this forever (seen live: it starved the WiFi/
                 * UDP stack badly enough to hang the web UI). Back off hard;
                 * still retries eventually in case the server-side password
                 * changes without a device reboot. */
                L->reg_refresh_at_us = esp_timer_get_time() + (int64_t)REG_FAIL_BACKOFF_S * 1000000;
                ESP_LOGW(TAG, "line %d: REGISTER auth rejected (%d) -- wrong SIP password? "
                              "set it with 'sip_set'. Backing off %ds.", line, code, REG_FAIL_BACKOFF_S);
                emit(line, VOIP_EV_REGISTER_FAILED, NULL);
                return;
            }
            char www[400];
            if (get_header(msg, "WWW-Authenticate", NULL, www, sizeof(www)) ||
                get_header(msg, "Proxy-Authenticate", NULL, www, sizeof(www))) {
                char uri[80], auth[512];
                snprintf(uri, sizeof(uri), "sip:%s", s_server);
                build_authorization(L, "REGISTER", uri, www, auth, sizeof(auth),
                                     code == 407 ? "Proxy-Authorization" : "Authorization");
                L->reg_authed_once = true;
                send_register(line, auth, REG_AUTH_RETRY_SAFETY_S);
            }
        } else if (code >= 200 && code < 300) {
            if (!L->registered) { L->registered = true; emit(line, VOIP_EV_REGISTERED, NULL); }
            L->reg_refresh_at_us = esp_timer_get_time() + (int64_t)(REG_EXPIRES / 2) * 1000000;
            L->reg_authed_once = false;
            ESP_LOGI(TAG, "line %d: registered as '%s' @ %s", line, L->user, s_server);
        } else if (code >= 300) {
            L->registered = false;
            emit(line, VOIP_EV_REGISTER_FAILED, NULL);
            ESP_LOGW(TAG, "line %d: REGISTER failed %d", line, code);
        }
        return;
    }

    if (is_inv && L->state == CALL_OUTGOING) {
        if (code == 401 || code == 407) {
            if (L->call_authed_once) { reset_call(L); emit(line, VOIP_EV_CLOSED, NULL); return; }
            char www[400], to[160];
            get_header(msg, "To", "t", to, sizeof(to)); get_tag(to, L->to_tag, sizeof(L->to_tag));
            send_ack(line, false); /* ACK the 401 on the INVITE transaction (same branch) */
            if (get_header(msg, "WWW-Authenticate", NULL, www, sizeof(www)) ||
                get_header(msg, "Proxy-Authenticate", NULL, www, sizeof(www))) {
                char auth[512];
                build_authorization(L, "INVITE", L->dial_uri, www, auth, sizeof(auth),
                                     code == 407 ? "Proxy-Authorization" : "Authorization");
                L->call_authed_once = true;
                rand_hex(L->branch, sizeof(L->branch));
                L->to_tag[0] = 0;
                send_invite(line, auth);
            }
        } else if (code >= 200 && code < 300) {
            char to[160], contact[160];
            get_header(msg, "To", "t", to, sizeof(to)); get_tag(to, L->to_tag, sizeof(L->to_tag));
            if (get_header(msg, "Contact", "m", contact, sizeof(contact)))
                get_uri(contact, L->remote_target, sizeof(L->remote_target));
            parse_sdp(msg, L->peer_rtp_ip, sizeof(L->peer_rtp_ip), &L->peer_rtp_port);
            send_ack(line, true); /* 2xx ACK: new transaction to peer target */
            if (L->peer_rtp_port && rtp_start(line, L->peer_rtp_ip, L->peer_rtp_port) == ESP_OK) {
                L->state = CALL_ACTIVE;
                emit(line, VOIP_EV_ANSWERED, NULL);
            } else {
                send_bye(line);
                reset_call(L);
                emit(line, VOIP_EV_CLOSED, NULL);
            }
        } else if (code >= 300) {
            char to[160];
            get_header(msg, "To", "t", to, sizeof(to)); get_tag(to, L->to_tag, sizeof(L->to_tag));
            send_ack(line, false); /* ACK the non-2xx final (same branch) */
            reset_call(L);
            emit(line, VOIP_EV_CLOSED, NULL);
            ESP_LOGW(TAG, "line %d: INVITE failed %d", line, code);
        }
    }
    /* 1xx and BYE/CANCEL responses: nothing to do. */
}

static void handle_request(const char *msg, const struct sockaddr_in *src)
{
    char method[16] = "";
    sscanf(msg, "%15s", method);
    char callid[80];
    if (!get_header(msg, "Call-ID", "i", callid, sizeof(callid))) return;

    if (strcmp(method, "OPTIONS") == 0) {
        line_t tmp = {0};
        get_header(msg, "Via", "v", tmp.in_via, sizeof(tmp.in_via));
        get_header(msg, "From", "f", tmp.in_from, sizeof(tmp.in_from));
        get_header(msg, "To", "t", tmp.in_to, sizeof(tmp.in_to));
        char cseq[64]; get_header(msg, "CSeq", NULL, cseq, sizeof(cseq)); tmp.in_cseq = atoi(cseq);
        strncpy(tmp.call_id, callid, sizeof(tmp.call_id) - 1);
        tmp.reply_addr = *src;
        send_response(&tmp, 200, "OK", "OPTIONS", false, 0);
        return;
    }

    if (strcmp(method, "INVITE") == 0) {
        /* Re-INVITE (same Call-ID as an already-established call on one of
         * our lines) vs. a genuinely new call: MUST be checked before the
         * ruri_user/CALL_IDLE logic below, which has no way to tell "this
         * call is already ours, being renegotiated" from "someone else is
         * calling while I'm busy" -- both look like "line busy" to it, and
         * it used to answer a re-INVITE with 486 Busy Here into the very
         * dialog it was renegotiating. Asterisk commonly sends one of
         * these seconds after answer (its own default direct_media/
         * session-timer behavior); we don't support renegotiating, so just
         * re-confirm the SDP we already answered with -- same RTP port,
         * nothing actually changes -- which is the standard way a device
         * that can't renegotiate mid-call satisfies this without dropping
         * the call. */
        int re_line = line_for_callid(callid);
        if (re_line >= 0) {
            line_t *L = &s_l[re_line];
            get_header(msg, "Via", "v", L->in_via, sizeof(L->in_via));
            get_header(msg, "From", "f", L->in_from, sizeof(L->in_from));
            get_header(msg, "To", "t", L->in_to, sizeof(L->in_to));
            char cseq[64]; get_header(msg, "CSeq", NULL, cseq, sizeof(cseq)); L->in_cseq = atoi(cseq);
            L->reply_addr = *src;
            send_response(L, 200, "OK", "INVITE", true, re_line);
            ESP_LOGI(TAG, "line %d: re-INVITE, re-confirmed existing SDP", re_line);
            return;
        }

        char ruri_user[40] = "", to[160], from[160];
        const char *u = strchr(msg, ':'); /* "INVITE sip:USER@host ..." */
        if (u) { u++; size_t i = 0; while (u[i] && u[i] != '@' && i < sizeof(ruri_user) - 1) { ruri_user[i] = u[i]; i++; } ruri_user[i] = 0; }
        int line = line_for_user(ruri_user);
        if (line < 0) line = 0; /* best effort on single-account boxes */
        line_t *L = &s_l[line];

        if (L->state != CALL_IDLE) {
            line_t t = {0};
            get_header(msg, "Via", "v", t.in_via, sizeof(t.in_via));
            get_header(msg, "From", "f", t.in_from, sizeof(t.in_from));
            get_header(msg, "To", "t", t.in_to, sizeof(t.in_to));
            char cseq[64]; get_header(msg, "CSeq", NULL, cseq, sizeof(cseq)); t.in_cseq = atoi(cseq);
            strncpy(t.call_id, callid, sizeof(t.call_id) - 1);
            t.reply_addr = *src;
            send_response(&t, 486, "Busy Here", "INVITE", false, line);
            return;
        }

        strncpy(L->call_id, callid, sizeof(L->call_id) - 1);
        get_header(msg, "Via", "v", L->in_via, sizeof(L->in_via));
        get_header(msg, "From", "f", L->in_from, sizeof(L->in_from));
        get_header(msg, "To", "t", to, sizeof(to));
        get_header(msg, "From", "f", from, sizeof(from));
        get_uri(from, L->remote_uri, sizeof(L->remote_uri));
        get_display_name(from, L->remote_name, sizeof(L->remote_name));
        char contact[160];
        if (get_header(msg, "Contact", "m", contact, sizeof(contact)))
            get_uri(contact, L->remote_target, sizeof(L->remote_target));
        get_tag(from, L->to_tag, sizeof(L->to_tag)); /* peer's tag (their From) */
        char cseq[64]; get_header(msg, "CSeq", NULL, cseq, sizeof(cseq)); L->in_cseq = atoi(cseq);
        rand_hex(L->from_tag, sizeof(L->from_tag));   /* our tag */
        /* Build via a local temp: snprintf'ing L->in_to from L->from_tag (both
         * inside s_l) trips -Wrestrict. */
        char to_tagged[224];
        snprintf(to_tagged, sizeof(to_tagged), "%s;tag=%s", to, L->from_tag);
        strncpy(L->in_to, to_tagged, sizeof(L->in_to) - 1);
        L->in_to[sizeof(L->in_to) - 1] = '\0';
        parse_sdp(msg, L->peer_rtp_ip, sizeof(L->peer_rtp_ip), &L->peer_rtp_port);
        L->reply_addr = *src;
        L->state = CALL_INCOMING;

        send_response(L, 100, "Trying", "INVITE", false, line);
        send_response(L, 180, "Ringing", "INVITE", false, line);
        emit(line, VOIP_EV_INCOMING, L->remote_uri);
        ESP_LOGI(TAG, "line %d: incoming call from %s%s%s%s", line,
                 L->remote_name[0] ? "\"" : "", L->remote_name,
                 L->remote_name[0] ? "\" " : "", L->remote_uri);
        return;
    }

    /* In-dialog request: match by Call-ID. */
    int line = line_for_callid(callid);
    if (line < 0) return;
    line_t *L = &s_l[line];
    get_header(msg, "Via", "v", L->in_via, sizeof(L->in_via));
    get_header(msg, "From", "f", L->in_from, sizeof(L->in_from));
    get_header(msg, "To", "t", L->in_to, sizeof(L->in_to));
    char cseq[64]; get_header(msg, "CSeq", NULL, cseq, sizeof(cseq)); L->in_cseq = atoi(cseq);
    L->reply_addr = *src;

    if (strcmp(method, "ACK") == 0) {
        return; /* media already up from our 200 OK */
    }
    if (strcmp(method, "BYE") == 0) {
        send_response(L, 200, "OK", "BYE", false, line);
        rtp_stop(line);
        reset_call(L);
        emit(line, VOIP_EV_CLOSED, NULL);
        ESP_LOGI(TAG, "line %d: remote hung up (BYE)", line);
        return;
    }
    if (strcmp(method, "CANCEL") == 0) {
        send_response(L, 200, "OK", "CANCEL", false, line);
        send_response(L, 487, "Request Terminated", "INVITE", false, line);
        rtp_stop(line);
        reset_call(L);
        emit(line, VOIP_EV_CLOSED, NULL);
        return;
    }
}

/* ---------- server resolution + main task ---------- */

/* Shared by resolve_server() (the configured account's server, cached into
 * s_server_ip) and voip_probe_number() (an arbitrary candidate server/number
 * check that must NOT disturb that cached state -- it may be run before
 * VoIP is even configured, or against a different host entirely). */
static bool resolve_host(const char *host, char *ip_out, size_t ip_out_len)
{
    struct in_addr a;
    if (inet_aton(host, &a)) { strncpy(ip_out, host, ip_out_len - 1); ip_out[ip_out_len - 1] = 0; return true; }

    size_t hl = strlen(host);
    if (hl > 6 && strcasecmp(host + hl - 6, ".local") == 0) {
        char base[64];
        strncpy(base, host, sizeof(base) - 1); base[sizeof(base) - 1] = 0;
        base[hl - 6] = '\0';
        esp_ip4_addr_t ip;
        if (mdns_query_a(base, 2000, &ip) == ESP_OK) {
            snprintf(ip_out, ip_out_len, IPSTR, IP2STR(&ip));
            return true;
        }
        ESP_LOGW(TAG, "mDNS could not resolve %s", host);
        return false;
    }

    struct hostent *he = gethostbyname(host);
    if (he && he->h_addr_list[0]) {
        struct in_addr *ia = (struct in_addr *)he->h_addr_list[0];
        strncpy(ip_out, inet_ntoa(*ia), ip_out_len - 1); ip_out[ip_out_len - 1] = 0;
        return true;
    }
    ESP_LOGW(TAG, "could not resolve host '%s'", host);
    return false;
}

static bool resolve_server(void)
{
    if (!resolve_host(s_server, s_server_ip, sizeof(s_server_ip))) return false;
    memset(&s_server_addr, 0, sizeof(s_server_addr));
    s_server_addr.sin_family = AF_INET;
    s_server_addr.sin_port = htons(SIP_PORT);
    s_server_addr.sin_addr.s_addr = inet_addr(s_server_ip);
    ESP_LOGI(TAG, "SIP server %s -> %s:%d", s_server, s_server_ip, SIP_PORT);
    return true;
}

static void register_all(void)
{
    for (int i = 0; i < NLINES; i++) {
        if (!s_l[i].enabled || !s_l[i].user[0]) continue;
        rand_hex(s_l[i].from_tag, sizeof(s_l[i].from_tag));
        rand_hex(s_l[i].call_id, sizeof(s_l[i].call_id));
        s_l[i].reg_authed_once = false;
        ESP_LOGI(TAG, "REGISTER -> %s:%d as '%s'", s_server_ip, SIP_PORT, s_l[i].user);
        send_register(i, NULL, REG_RETRY_S);
    }
}

static void sip_task(void *arg)
{
    while (!wifi_is_connected()) vTaskDelay(pdMS_TO_TICKS(500));
    wifi_get_ip_str(s_myip, sizeof(s_myip));

    if (!s_server[0]) {
        ESP_LOGW(TAG, "no SIP server configured -- run 'sip_set' from the console");
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    s_sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in local = { .sin_family = AF_INET, .sin_port = htons(SIP_PORT),
                                 .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (s_sock < 0 || bind(s_sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        ESP_LOGE(TAG, "SIP socket bind :%d failed", SIP_PORT);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    struct timeval tv = { .tv_sec = 0, .tv_usec = 300000 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (resolve_server()) register_all();

    char *msg = malloc(MSG_MAX);
    if (!msg) { vTaskDelete(NULL); return; }
    int64_t next_resolve = 0;
    while (1) {
        struct sockaddr_in src; socklen_t sl = sizeof(src);
        int n = recvfrom(s_sock, msg, MSG_MAX - 1, 0, (struct sockaddr *)&src, &sl);
        if (n > 0) {
            msg[n] = '\0';
            char first[80]; size_t fl = 0;
            while (fl < sizeof(first) - 1 && msg[fl] && msg[fl] != '\r') { first[fl] = msg[fl]; fl++; }
            first[fl] = '\0';
            ESP_LOGI(TAG, "rx %d from %s: %s", n, inet_ntoa(src.sin_addr), first);
            if (strncmp(msg, "SIP/2.0", 7) == 0) handle_response(msg, &src);
            else handle_request(msg, &src);
        }
        int64_t now = esp_timer_get_time();
        for (int i = 0; i < NLINES; i++) {
            /* Not gated on s_l[i].registered: this timer now also covers
             * initial registration (register_all() arms it the same way
             * send_register() always does), so a line stuck never having
             * registered at all -- e.g. its very first REGISTER's response
             * lost -- retries instead of waiting forever. */
            if (s_l[i].reg_refresh_at_us && now >= s_l[i].reg_refresh_at_us) {
                s_l[i].reg_refresh_at_us = 0;
                s_l[i].reg_authed_once = false;
                send_register(i, NULL, REG_RETRY_S);
            }
        }
        if (!s_server_ip[0] && now >= next_resolve) {
            next_resolve = now + 5000000;
            if (resolve_server()) register_all();
        }
    }
    free(msg);
}

/* ---------- config ---------- */

static void load_config(void)
{
    /* No default server -- ships unconfigured, like CONFIG_WIFI_SSID.
     * Set via the web UI or `sip_set` (see bringup.c). */
    strncpy(s_server, "", sizeof(s_server) - 1);
    strncpy(s_l[0].user, "phone", sizeof(s_l[0].user) - 1);
    strncpy(s_l[1].user, "phone2", sizeof(s_l[1].user) - 1);
    s_l[0].enabled = s_l[1].enabled = true;

    nvs_handle_t h;
    if (nvs_open(VOIP_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz;
    sz = sizeof(s_server);   nvs_get_str(h, "server", s_server, &sz);
    sz = sizeof(s_l[0].user); nvs_get_str(h, "user0", s_l[0].user, &sz);
    sz = sizeof(s_l[0].pass); nvs_get_str(h, "pass0", s_l[0].pass, &sz);
    sz = sizeof(s_l[1].user); nvs_get_str(h, "user1", s_l[1].user, &sz);
    sz = sizeof(s_l[1].pass); nvs_get_str(h, "pass1", s_l[1].pass, &sz);
    /* NVS key kept as "provpass" (its name from before the auto-provisioner
     * flow was removed) so a board upgraded from an older build doesn't
     * silently lose an already-saved secret. */
    sz = sizeof(s_setup_pass); nvs_get_str(h, "provpass", s_setup_pass, &sz);
    nvs_close(h);
}

/* ---------- public API (voip.h) ---------- */

esp_err_t voip_init(voip_event_cb cb, void *ctx)
{
    s_cb = cb;
    s_ctx = ctx;
    for (int i = 0; i < NLINES; i++) reset_call(&s_l[i]);
    load_config();
    ESP_LOGI(TAG, "VoIP UA init (server '%s', users '%s'/'%s')",
             s_server, s_l[0].user, s_l[1].user);
    /* 8 KB: the send_*() helpers each put a 1600-byte SIP datagram on the
     * stack, called nested inside the recvfrom handlers. */
    xTaskCreate(sip_task, "sip", 8192, NULL, 5, &s_task);
    return ESP_OK;
}

esp_err_t voip_register(const voip_account_t *acct)
{
    if (acct && acct->line >= 0 && acct->line < NLINES && s_server_ip[0]) {
        send_register(acct->line, NULL, REG_RETRY_S);
        return ESP_OK;
    }
    return ESP_ERR_INVALID_STATE;
}

bool voip_is_registered(int line)
{
    return (line >= 0 && line < NLINES) && s_l[line].registered;
}

esp_err_t voip_dial(int line, const char *number)
{
    if (line < 0 || line >= NLINES) return ESP_ERR_INVALID_ARG;
    if (!s_server_ip[0]) return ESP_ERR_INVALID_STATE;
    line_t *L = &s_l[line];
    if (L->state != CALL_IDLE) return ESP_ERR_INVALID_STATE;

    /* "555xxxx" -> extension "xxxx" (matches the Pi's 555-prefix scheme). */
    const char *ext = number;
    if (strncmp(number, "555", 3) == 0 && strlen(number) > 3) ext = number + 3;
    snprintf(L->dial_uri, sizeof(L->dial_uri), "sip:%s@%s", ext, s_server);
    strncpy(L->remote_uri, L->dial_uri, sizeof(L->remote_uri) - 1);

    rand_hex(L->call_id, sizeof(L->call_id));
    rand_hex(L->from_tag, sizeof(L->from_tag));
    rand_hex(L->branch, sizeof(L->branch));
    L->to_tag[0] = 0;
    L->cseq = 0;
    L->call_authed_once = false;
    L->state = CALL_OUTGOING;
    send_invite(line, NULL);
    ESP_LOGI(TAG, "line %d: dialing %s", line, L->dial_uri);
    return ESP_OK;
}

esp_err_t voip_answer(int line)
{
    if (line < 0 || line >= NLINES) return ESP_ERR_INVALID_ARG;
    line_t *L = &s_l[line];
    if (L->state != CALL_INCOMING) return ESP_ERR_INVALID_STATE;
    send_response(L, 200, "OK", "INVITE", true, line);
    if (L->peer_rtp_port && rtp_start(line, L->peer_rtp_ip, L->peer_rtp_port) == ESP_OK) {
        L->state = CALL_ACTIVE;
        emit(line, VOIP_EV_ANSWERED, NULL);
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t voip_hangup(int line)
{
    if (line < 0 || line >= NLINES) return ESP_ERR_INVALID_ARG;
    line_t *L = &s_l[line];
    switch (L->state) {
        /* Answered call: BYE. Still-ringing outbound call: CANCEL, not BYE
         * -- BYE only applies to an established dialog (needs a to_tag
         * from a final response we don't have yet); sending it here used
         * to produce a malformed request Asterisk couldn't match to
         * anything, so it never learned we hung up and just left the
         * callee ringing until answered or its own timeout. */
        case CALL_ACTIVE:   send_bye(line);    break;
        case CALL_OUTGOING: send_cancel(line); break;
        case CALL_INCOMING: send_response(L, 603, "Decline", "INVITE", false, line); break;
        default: break;
    }
    rtp_stop(line);
    reset_call(L);
    return ESP_OK;
}

const char *voip_peer_name(int line)
{
    if (line < 0 || line >= NLINES) return "";
    return s_l[line].remote_name;
}

/* Static scratch, not a line_t field: the user part is only ever needed
 * transiently (telephony copies it into its own state on the ringing
 * event), so deriving it on demand beats carrying another buffer per line
 * that has to be kept in sync with remote_uri. */
const char *voip_peer_number(int line)
{
    static char num[40];
    if (line < 0 || line >= NLINES || !s_l[line].remote_uri[0]) return "";
    uri_user(s_l[line].remote_uri, num, sizeof(num));
    return num;
}

bool voip_is_stub(void) { return false; }

const char *voip_server(void) { return s_server; }

const char *voip_user(int line)
{
    if (line < 0 || line >= NLINES) return "";
    return s_l[line].user;
}

const char *voip_pass(int line)
{
    if (line < 0 || line >= NLINES) return "";
    return s_l[line].pass;
}

const char *voip_setup_password(void) { return s_setup_pass; }

/* ---------- runtime config setter (console/web) ---------- */

esp_err_t voip_set_config(const char *server, const char *user0, const char *pass0,
                          const char *user1, const char *pass1)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(VOIP_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (server) nvs_set_str(h, "server", server);
    if (user0)  nvs_set_str(h, "user0", user0);
    if (pass0)  nvs_set_str(h, "pass0", pass0);
    if (user1)  nvs_set_str(h, "user1", user1);
    if (pass1)  nvs_set_str(h, "pass1", pass1);
    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "VoIP config saved; reboot to apply");
    return err;
}

esp_err_t voip_set_setup_password(const char *password)
{
    if (!password) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(VOIP_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str(h, "provpass", password); /* key name: see load_config()'s comment */
    strncpy(s_setup_pass, password, sizeof(s_setup_pass) - 1);
    s_setup_pass[sizeof(s_setup_pass) - 1] = '\0';
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ---------- number-availability probe (web UI: check before you commit) ----------
 *
 * Fires a standalone SIP OPTIONS at sip:<number>@<server> from a throwaway
 * socket/port -- entirely separate from s_sock/sip_task's registered lines,
 * so it's safe to call before VoIP is even configured, or against a server
 * that isn't the one currently in use.
 *
 * Interpreting the result is inherently best-effort: this project uses one
 * value (voip_user()) as both the SIP username and the dialable extension,
 * so "is this number free" really means "does an endpoint/AOR by this name
 * already exist". A clean 404 from Asterisk is a reliable "no such thing" --
 * anything else (2xx, 401/403 challenging for auth, a redirect, or no
 * response at all within the timeout) is reported as NOT confirmed free,
 * since a false "it's free" is the failure mode that actually causes a
 * collision. This is a convenience check, not a substitute for confirming
 * on the server itself (`asterisk -rx "pjsip show endpoint <number>"`). */
esp_err_t voip_probe_number(const char *server, const char *number, voip_probe_result_t *out_result)
{
    *out_result = VOIP_PROBE_UNKNOWN;
    if (!server || !server[0] || !number || !number[0]) return ESP_ERR_INVALID_ARG;

    char ip[16];
    if (!resolve_host(server, ip, sizeof(ip))) return ESP_ERR_NOT_FOUND;

    char my_ip[16];
    wifi_get_ip_str(my_ip, sizeof(my_ip));
    if (!my_ip[0] || strcmp(my_ip, "0.0.0.0") == 0) return ESP_ERR_INVALID_STATE;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return ESP_ERR_NO_MEM;
    struct sockaddr_in local = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) { close(sock); return ESP_FAIL; }
    socklen_t local_len = sizeof(local);
    getsockname(sock, (struct sockaddr *)&local, &local_len);
    uint16_t my_port = ntohs(local.sin_port);

    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(SIP_PORT) };
    to.sin_addr.s_addr = inet_addr(ip);

    char branch[40], from_tag[24], call_id[32];
    rand_hex(branch, sizeof(branch));
    rand_hex(from_tag, sizeof(from_tag));
    rand_hex(call_id, sizeof(call_id));
    char buf[MSG_MAX];
    int n = snprintf(buf, sizeof(buf),
        "OPTIONS sip:%s@%s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=z9hG4bK%s;rport\r\n"
        "Max-Forwards: 70\r\n"
        "From: <sip:probe@%s>;tag=%s\r\n"
        "To: <sip:%s@%s>\r\n"
        "Call-ID: %s\r\n"
        "CSeq: 1 OPTIONS\r\n"
        "Contact: <sip:probe@%s:%d>\r\n"
        "User-Agent: RotaryPhone-ESP32\r\n"
        "Content-Length: 0\r\n\r\n",
        number, server,
        my_ip, my_port, branch,
        server, from_tag,
        number, server,
        call_id,
        my_ip, my_port);
    if (n <= 0 || n >= (int)sizeof(buf)) { close(sock); return ESP_ERR_INVALID_SIZE; }
    sendto(sock, buf, n, 0, (const struct sockaddr *)&to, sizeof(to));

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int64_t deadline = esp_timer_get_time() + 2000000;
    int final_code = 0;
    while (esp_timer_get_time() < deadline) {
        int r = recvfrom(sock, buf, sizeof(buf) - 1, 0, NULL, NULL);
        if (r <= 0) break; /* timeout or error -- no more coming */
        buf[r] = '\0';
        if (strncmp(buf, "SIP/2.0 ", 8) != 0) continue; /* not a status line */
        int code = atoi(buf + 8);
        if (code >= 200) { final_code = code; break; } /* final response; stop */
        /* 1xx provisional -- keep waiting for the final response */
    }
    close(sock);

    if (final_code == 404) *out_result = VOIP_PROBE_FREE;
    else if (final_code > 0) *out_result = VOIP_PROBE_TAKEN;
    else *out_result = VOIP_PROBE_UNKNOWN;
    ESP_LOGI(TAG, "probe %s@%s -> SIP %d (%s)", number, server, final_code,
             *out_result == VOIP_PROBE_FREE ? "free" :
             *out_result == VOIP_PROBE_TAKEN ? "taken" : "unknown");
    return ESP_OK;
}
