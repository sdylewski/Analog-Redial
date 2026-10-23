/* Host-side checks of bt401_proto.c against the BT401 datasheet's example
 * lines and the DFRobot library's command strings. */
#include "bt401_proto.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static bool feed_str(bt401_parser_t *p, const char *s, bt401_msg_t *last)
{
    bool got = false;
    for (; *s; s++) if (bt401_parser_feed(p, (uint8_t)*s, last)) got = true;
    return got;
}

static void test_build(void)
{
    char b[48];
    CHECK(bt401_build_cmd(BT401_CMD_ANSWER, NULL, b, sizeof(b)) == 9 && !strcmp(b, "AT+BA04\r\n"));
    CHECK(bt401_build_cmd("CA", "20", b, sizeof(b)) && !strcmp(b, "AT+CA20\r\n"));      /* datasheet example */
    CHECK(bt401_build_cmd(BT401_CMD_QUERY_STATUS, NULL, b, sizeof(b)) && !strcmp(b, "AT+TS\r\n"));
    CHECK(bt401_build_cmd("BA04", NULL, b, 5) == 0);                                     /* too small */
    CHECK(bt401_build_dial("10086", b, sizeof(b)) && !strcmp(b, "AT+BT10086\r\n"));      /* datasheet example */
    CHECK(bt401_build_dial("13510250437", b, sizeof(b)) && !strcmp(b, "AT+BT13510250437\r\n"));
    CHECK(bt401_build_dial("", b, sizeof(b)) == 0);
    CHECK(bt401_build_dial(NULL, b, sizeof(b)) == 0);
    CHECK(bt401_build_dial("12a4", b, sizeof(b)) == 0);
    CHECK(bt401_build_dial("12345678901234567890", b, sizeof(b)) == 0);                  /* 20 digits */
    CHECK(bt401_build_dial("1234567890123456789", b, sizeof(b)) != 0);                   /* 19 digits */
}

static void test_parse(void)
{
    bt401_parser_t p; bt401_msg_t m;
    bt401_parser_init(&p);

    CHECK(feed_str(&p, "OK\r\n", &m) && m.kind == BT401_MSG_OK);
    CHECK(feed_str(&p, "TS+04\r\n", &m) && m.kind == BT401_MSG_STATUS && m.num == 4);
    CHECK(feed_str(&p, "TT+13510250437\r\n", &m) && m.kind == BT401_MSG_NUMBER && !strcmp(m.text, "13510250437"));
    CHECK(feed_str(&p, "TD+BT201-AUDIO\r\n", &m) && m.kind == BT401_MSG_NAME && !strcmp(m.text, "BT201-AUDIO"));
    CHECK(feed_str(&p, "TA+9EE884AB8BCC\r\n", &m) && m.kind == BT401_MSG_ADDR && !strcmp(m.text, "9E:E8:84:AB:8B:CC"));
    CHECK(feed_str(&p, "QA+30\r\n", &m) && m.kind == BT401_MSG_VOLUME && m.num == 30);
    CHECK(feed_str(&p, "QM+00\r\n", &m) && m.kind == BT401_MSG_MODE && m.num == 0);
    CHECK(feed_str(&p, "ER+4\r\n", &m) && m.kind == BT401_MSG_ERROR && m.num == 4);
    CHECK(feed_str(&p, "T2+01\r\n", &m) && m.kind == BT401_MSG_OTHER && !strcmp(m.code, "T2"));
    CHECK(feed_str(&p, "TS+x\r\n", &m) && m.kind == BT401_MSG_OTHER);                    /* not a number */

    /* LF-only endings work; blank lines and junk are skipped or ignored. */
    CHECK(feed_str(&p, "TS+01\n", &m) && m.num == 1);
    CHECK(!feed_str(&p, "\r\n\r\n", &m));
    CHECK(feed_str(&p, "hello\r\n", &m) && m.kind == BT401_MSG_NONE);

    /* A line split across reads. */
    CHECK(!feed_str(&p, "TS+0", &m));
    CHECK(feed_str(&p, "3\r\n", &m) && m.num == 3);

    /* Over-long line is dropped whole, and the next one still parses. */
    char big[120]; memset(big, 'A', sizeof(big)); big[sizeof(big) - 1] = 0;
    CHECK(!feed_str(&p, big, &m));
    CHECK(!feed_str(&p, "\r\n", &m));
    CHECK(p.overflows == 1);
    CHECK(feed_str(&p, "TS+02\r\n", &m) && m.num == 2);
}

static void apply(bt401_state_t *s, const char *line, unsigned *chg)
{
    bt401_msg_t m;
    bt401_decode_line(line, &m);
    *chg = bt401_state_apply(s, &m);
}

static void test_state(void)
{
    bt401_state_t s; unsigned c;
    bt401_state_init(&s);
    CHECK(s.ts == -1 && !s.connected && s.call == BT401_CALL_IDLE);

    apply(&s, "TS+00", &c);
    CHECK(!s.connected && c == 0 && s.ts == 0);
    apply(&s, "TS+01", &c);
    CHECK(s.connected && (c & BT401_CHG_LINK));

    /* incoming call: ring, number, answer, hang up */
    apply(&s, "TS+03", &c);
    CHECK(s.call == BT401_CALL_INCOMING && (c & BT401_CHG_CALL));
    apply(&s, "TT+5551234", &c);
    CHECK((c & BT401_CHG_CALLER) && !strcmp(s.caller_id, "5551234"));
    apply(&s, "TT+5551234", &c);
    CHECK(c == 0);                                   /* repeated every 2 s: no change */
    apply(&s, "TS+04", &c);
    CHECK(s.call == BT401_CALL_ACTIVE && !strcmp(s.caller_id, "5551234"));
    apply(&s, "TS+01", &c);
    CHECK(s.call == BT401_CALL_IDLE && s.caller_id[0] == 0 && (c & BT401_CHG_CALL) && (c & BT401_CHG_CALLER));
    CHECK(s.connected);

    /* outgoing call: the driver sets dialing before it sends the dial command */
    s.dialing = true;
    apply(&s, "TS+03", &c);
    CHECK(s.call == BT401_CALL_OUTGOING);
    apply(&s, "TS+04", &c);
    CHECK(s.call == BT401_CALL_ACTIVE);
    apply(&s, "TS+01", &c);
    CHECK(s.call == BT401_CALL_IDLE && !s.dialing);

    /* a refused dial clears the flag */
    s.dialing = true;
    apply(&s, "ER+3", &c);
    CHECK(!s.dialing && s.errors == 1);
    /* ...but an error during a call doesn't touch the call */
    apply(&s, "TS+04", &c);
    apply(&s, "ER+4", &c);
    CHECK(s.call == BT401_CALL_ACTIVE);

    /* music */
    apply(&s, "TS+01", &c);
    apply(&s, "TS+02", &c);
    CHECK(s.music && s.connected && s.call == BT401_CALL_IDLE && (c & BT401_CHG_MUSIC));
    apply(&s, "TS+01", &c);
    CHECK(!s.music);

    /* link lost */
    apply(&s, "TS+00", &c);
    CHECK(!s.connected && (c & BT401_CHG_LINK));

    apply(&s, "TD+BT201-AUDIO", &c);
    apply(&s, "TA+9EE884AB8BCC", &c);
    CHECK(!strcmp(s.name, "BT201-AUDIO") && !strcmp(s.addr, "9E:E8:84:AB:8B:CC") && (c & BT401_CHG_INFO));
}

static void test_describe(void)
{
    bt401_msg_t m; char d[64];
    bt401_decode_line("TS+04", &m);
    bt401_describe(&m, d, sizeof(d));
    CHECK(strstr(d, "in a call") != NULL);
    bt401_decode_line("ER+2", &m);
    bt401_describe(&m, d, sizeof(d));
    CHECK(strstr(d, "error 2") != NULL);
}

int main(void)
{
    test_build();
    test_parse();
    test_state();
    test_describe();
    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("bt401_proto: all checks passed\n");
    return 0;
}
