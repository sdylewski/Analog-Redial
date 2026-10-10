/* Host-side tests for components/bt_companion/bk3266_proto.c.
 *
 * Run: firmware/test/host/run.sh   (any C compiler; no ESP-IDF needed)
 *
 * The example frames below are the ones in the vendor's protocol
 * spreadsheet (Bluetooth/BK3266/reference_docs/13618 BK3266SR Commands.xls).
 * What these tests can't show is how a real module behaves -- that's what
 * archive/hardware/bk3266.md tracks. */
#include "bk3266_proto.h"

#include <stdio.h>
#include <string.h>

static int g_fail, g_run;

#define CHECK(cond) do { g_run++; if (!(cond)) { g_fail++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static bool feed_all(bk_parser_t *p, const uint8_t *b, size_t n, bk_frame_t *last, int *count)
{
    bool any = false;
    for (size_t i = 0; i < n; i++) {
        bk_frame_t f;
        if (bk_parser_feed(p, b[i], &f)) { *last = f; (*count)++; any = true; }
    }
    return any;
}

static void status_frame(uint8_t st, uint8_t *out, size_t *n)
{
    *n = bk_build_frame(BK_MID_ACK, &st, 1, out, 32);
}

static unsigned apply_status(bk_state_t *s, uint8_t st)
{
    bk_frame_t f = { .cmd = BK_MID_ACK, .n = 1, .data = { st } };
    return bk_state_apply(s, &f);
}

static void test_build(void)
{
    uint8_t out[32];
    static const uint8_t pair[] = { 0x52,0x42,0x23,0x04,0x01,0x01,0x06,0x23,0x42,0x52 };
    size_t n = bk_build_command(BK_CMD_MATCH, out, sizeof(out));
    CHECK(n == sizeof(pair) && memcmp(out, pair, n) == 0);

    /* the spreadsheet's "send a device address" example */
    static const uint8_t addr[] = { 0x3D,0xAB,0x55,0xFA,0x58,0xFC };
    static const uint8_t want[] = { 0x52,0x42,0x23,0x09,0x0B,0x3D,0xAB,0x55,0xFA,0x58,0xFC,0x9F,0x23,0x42,0x52 };
    n = bk_build_frame(0x0B, addr, sizeof(addr), out, sizeof(out));
    CHECK(n == sizeof(want) && memcmp(out, want, n) == 0);

    CHECK(bk_build_frame(1, addr, sizeof(addr), out, 5) == 0);          /* buffer too small */
    CHECK(bk_build_frame(1, addr, BK_FRAME_MAX_DATA + 1, out, 200) == 0); /* too much data */
}

static void test_parse(void)
{
    bk_parser_t p; bk_frame_t f; int count = 0;
    bk_parser_init(&p);

    static const uint8_t ok[] = { 0x52,0x42,0x23,0x04,0x00,0x61,0x65,0x23,0x42,0x52 };
    CHECK(feed_all(&p, ok, sizeof(ok), &f, &count));
    CHECK(count == 1 && f.cmd == BK_MID_ACK && f.n == 1 && f.data[0] == BK_ST_OK);

    /* a frame with no data at all: LEN 3 */
    uint8_t empty[16]; size_t n = bk_build_frame(0x55, NULL, 0, empty, sizeof(empty));
    count = 0;
    CHECK(feed_all(&p, empty, n, &f, &count) && count == 1 && f.cmd == 0x55 && f.n == 0);

    /* garbage, a stray 0x52, then a good frame: resyncs and still finds it */
    static const uint8_t noisy[] = { 0x00,0xFF,0x52,0x52,0x42,0x99,
                                     0x52,0x42,0x23,0x04,0x00,0x73,0x77,0x23,0x42,0x52 };
    count = 0;
    CHECK(feed_all(&p, noisy, sizeof(noisy), &f, &count) && count == 1 && f.data[0] == BK_ST_IC);

    /* two frames back to back */
    uint8_t two[64]; size_t a, b;
    status_frame(BK_ST_ON, two, &a);
    status_frame(BK_ST_II, two + a, &b);
    count = 0;
    feed_all(&p, two, a + b, &f, &count);
    CHECK(count == 2 && f.data[0] == BK_ST_II);

    /* bad checksum: counted, not delivered */
    static const uint8_t badcrc[] = { 0x52,0x42,0x23,0x04,0x00,0x61,0x66,0x23,0x42,0x52 };
    uint32_t before = p.crc_errors;
    count = 0;
    CHECK(!feed_all(&p, badcrc, sizeof(badcrc), &f, &count) && p.crc_errors == before + 1);

    /* bad footer: dropped */
    static const uint8_t badftr[] = { 0x52,0x42,0x23,0x04,0x00,0x61,0x65,0x23,0x42,0x00 };
    count = 0;
    CHECK(!feed_all(&p, badftr, sizeof(badftr), &f, &count));

    /* absurd LEN is rejected rather than swallowing the next frame */
    static const uint8_t biglen[] = { 0x52,0x42,0x23,0xFF,
                                      0x52,0x42,0x23,0x04,0x00,0x61,0x65,0x23,0x42,0x52 };
    count = 0;
    CHECK(feed_all(&p, biglen, sizeof(biglen), &f, &count) && count == 1);

    /* a frame split across two reads */
    bk_parser_init(&p); count = 0;
    feed_all(&p, ok, 4, &f, &count);
    CHECK(count == 0);
    feed_all(&p, ok + 4, sizeof(ok) - 4, &f, &count);
    CHECK(count == 1);
}

static void test_state(void)
{
    bk_state_t s; bk_state_init(&s);
    CHECK(s.volume == -1 && !s.connected && s.call == BK_CALL_IDLE);

    unsigned c = apply_status(&s, BK_ST_ON);
    CHECK((c & BK_CHG_BOOT) && s.booted && !s.connected);

    c = apply_status(&s, BK_ST_II);
    CHECK((c & BK_CHG_LINK) && s.connected);
    CHECK(apply_status(&s, BK_ST_II) == 0);          /* repeat: no change */

    /* a call comes in; the number follows the status */
    c = apply_status(&s, BK_ST_IC);
    CHECK((c & BK_CHG_CALL) && s.call == BK_CALL_INCOMING);
    bk_frame_t num = { .cmd = BK_MID_IR, .n = 10 };
    memcpy(num.data, "4155550123", 10);
    c = bk_state_apply(&s, &num);
    CHECK((c & BK_CHG_CALL) && strcmp(s.caller_id, "4155550123") == 0);
    CHECK(bk_state_apply(&s, &num) == 0);            /* same number again: no change */

    c = apply_status(&s, BK_ST_M4);
    CHECK((c & BK_CHG_CALL) && s.call == BK_CALL_ACTIVE);
    CHECK(strcmp(s.caller_id, "4155550123") == 0);   /* kept while the call is up */

    c = apply_status(&s, BK_ST_M1);                  /* the query says: linked, no call */
    CHECK((c & BK_CHG_CALL) && s.call == BK_CALL_IDLE && s.caller_id[0] == '\0');

    CHECK((apply_status(&s, BK_ST_M3) & BK_CHG_CALL) && s.call == BK_CALL_OUTGOING);
    CHECK((apply_status(&s, BK_ST_ID) == 0) && s.call == BK_CALL_OUTGOING);

    /* the phone drops off mid-call */
    bk_frame_t pn = { .cmd = BK_MID_PHONENAME, .n = 6 };
    memcpy(pn.data, "iPhone", 6);
    bk_state_apply(&s, &pn);
    CHECK(strcmp(s.phone_name, "iPhone") == 0);
    c = apply_status(&s, BK_ST_IA);
    CHECK((c & BK_CHG_LINK) && (c & BK_CHG_CALL) && !s.connected &&
          s.call == BK_CALL_IDLE && s.phone_name[0] == '\0');

    /* own address: 6 raw bytes, or text */
    bk_frame_t ad = { .cmd = BK_MID_ADDR, .n = 6, .data = { 0x3D,0xAB,0x55,0xFA,0x58,0xFC } };
    bk_state_apply(&s, &ad);
    CHECK(strcmp(s.local_addr, "3D:AB:55:FA:58:FC") == 0);
    bk_frame_t at = { .cmd = BK_MID_ADDR, .n = 12 };
    memcpy(at.data, "3DAB55FA58FC", 12);
    bk_state_apply(&s, &at);
    CHECK(strcmp(s.local_addr, "3DAB55FA58FC") == 0);

    /* non-printable bytes from a phone can't reach the log or the web page */
    bk_frame_t bad = { .cmd = BK_MID_NAME, .n = 4, .data = { 'a', 0x01, '<', 'b' } };
    bk_state_apply(&s, &bad);
    CHECK(strcmp(s.local_name, "a?<b") == 0);

    /* too-long text is cut, not overflowed */
    bk_frame_t longname = { .cmd = BK_MID_NAME, .n = BK_FRAME_MAX_DATA };
    memset(longname.data, 'x', BK_FRAME_MAX_DATA);
    bk_state_apply(&s, &longname);
    CHECK(strlen(s.local_name) == BK_NAME_MAX - 1);

    bk_frame_t vol = { .cmd = BK_MID_VOL, .n = 1, .data = { 12 } };
    bk_state_apply(&s, &vol);
    CHECK(s.volume == 12);

    /* a reboot clears everything */
    apply_status(&s, BK_ST_II);
    apply_status(&s, BK_ST_IC);
    apply_status(&s, BK_ST_ON);
    CHECK(s.booted && !s.connected && s.call == BK_CALL_IDLE && s.volume == -1);
}

static void test_describe(void)
{
    char out[96];
    bk_frame_t f = { .cmd = BK_MID_ACK, .n = 1, .data = { BK_ST_IC } };
    bk_describe(&f, out, sizeof(out));
    CHECK(strstr(out, "0x73") && strstr(out, "incoming"));
    bk_frame_t u = { .cmd = 0x42, .n = 2 };
    bk_describe(&u, out, sizeof(out));
    CHECK(strstr(out, "0x42") && strstr(out, "2 data bytes"));
}

int main(void)
{
    test_build();
    test_parse();
    test_state();
    test_describe();
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
