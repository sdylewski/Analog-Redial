/* bt401_proto.h — the DFRobot DFR0781 / BT401 module's serial protocol, as pure C.
 *
 * No ESP-IDF dependencies on purpose: firmware/test/host/ compiles this on a
 * Mac and checks it against the example lines in the BT401 datasheet and
 * DFRobot's Arduino library (Bluetooth/DFR0781_BT401/), so framing and
 * state-mapping mistakes show up without the module. The UART and task side
 * is bt401.c.
 *
 * The protocol is text, 115200 8N1, every line ending in CRLF:
 *
 *   host -> module   AT+<cmd>[param]\r\n      e.g.  AT+BA04   answer
 *                                                    AT+BT5551234   dial
 *   module -> host   OK\r\n                   a control command was accepted
 *                    ER+<n>\r\n               it was not (n = 1..8)
 *                    <XX>+<value>\r\n         a query reply, or a status the
 *                                             module volunteers
 *
 * Replies the driver uses:
 *   TS+00  not connected, waiting for pairing      TT+<number>  the other party's number
 *   TS+01  connected, idle                         TD+<name>    the module's own name
 *   TS+02  connected, playing music                TA+<hex>     the module's own address
 *   TS+03  a call is ringing (out or in)           QA+<n>       volume, sent once at power-up
 *   TS+04  in a call                               QM+<n>       mode, sent once at power-up
 *
 * Verified against the datasheet (BT401 V1.5) and the library only; nothing
 * here has talked to a real module yet. archive/hardware/bt401.md lists what
 * to check first on the bench.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BT401_BAUD        115200
#define BT401_LINE_MAX    48     /* longest line kept; a longer one is dropped whole */
#define BT401_NUMBER_MAX  24
#define BT401_NAME_MAX    32
#define BT401_DIAL_MAX    19     /* digits accepted by bt401_build_dial() */

/* The commands the driver sends (the part after "AT+"). */
#define BT401_CMD_ANSWER        "BA04"   /* answer the call */
#define BT401_CMD_REJECT        "BA02"   /* reject the call */
#define BT401_CMD_HANGUP        "BA03"   /* hang up */
#define BT401_CMD_DISCONNECT    "BA01"   /* drop the Bluetooth link */
#define BT401_CMD_DISCOVERABLE  "BA06"   /* make the audio side discoverable (pairing) */
#define BT401_CMD_CALLS_ON      "B201"   /* enable HFP calls (stored by the module) */
#define BT401_CMD_DAC           "CS"     /* CS00 = DAC output high-impedance, CS01 = DAC on */
#define BT401_CMD_QUERY_STATUS  "TS"
#define BT401_CMD_QUERY_NUMBER  "TT"
#define BT401_CMD_QUERY_NAME    "TD"
#define BT401_CMD_QUERY_ADDR    "TA"
#define BT401_CMD_QUERY_MODE    "QM"     /* QM+01 = Bluetooth mode */
#define BT401_CMD_MODE_BT       "CM01"   /* Bluetooth mode */
#define BT401_CMD_MODE_IDLE     "CM08"   /* idle mode; radio off only with CG00 */
#define BT401_CMD_BG_ON         "CG01"   /* Bluetooth in the background (factory; stored, after a reset) */
#define BT401_CMD_BG_OFF        "CG00"   /* no background Bluetooth (stored, after a reset) */
#define BT401_CMD_RESET         "CZ"

typedef enum {
    BT401_MSG_NONE = 0,
    BT401_MSG_OK,        /* OK */
    BT401_MSG_ERROR,     /* ER+n:   num = n */
    BT401_MSG_STATUS,    /* TS+nn:  num = 0..4 */
    BT401_MSG_NUMBER,    /* TT+...: text = the number */
    BT401_MSG_NAME,      /* TD+...: text = the module's name */
    BT401_MSG_ADDR,      /* TA+...: text = "AA:BB:CC:DD:EE:FF" */
    BT401_MSG_VOLUME,    /* QA+nn:  num = level */
    BT401_MSG_MODE,      /* QM+nn:  num = mode */
    BT401_MSG_OTHER,     /* any other well-formed XX+value line */
} bt401_msg_kind_t;

typedef struct {
    bt401_msg_kind_t kind;
    int  num;
    char code[3];                   /* the two letters before the '+', "" for OK */
    char text[BT401_LINE_MAX];
} bt401_msg_t;

/* TS values. */
enum { BT401_TS_STANDBY = 0, BT401_TS_IDLE = 1, BT401_TS_MUSIC = 2, BT401_TS_RINGING = 3, BT401_TS_IN_CALL = 4 };

/* Builds "AT+<cmd><param>\r\n" into out (param may be NULL). Returns its
 * length, or 0 if it doesn't fit. */
size_t bt401_build_cmd(const char *cmd, const char *param, char *out, size_t out_max);

/* "AT+BT<number>\r\n". Returns 0 for an empty or over-long number, or one with
 * anything other than 0-9, '*', '#', '+'. */
size_t bt401_build_dial(const char *number, char *out, size_t out_max);

/* Byte-at-a-time line parser. A line is complete at '\n'; a '\r' is ignored,
 * a line over BT401_LINE_MAX is dropped (and counted), blank lines are skipped. */
typedef struct {
    char     buf[BT401_LINE_MAX];
    uint8_t  len;
    bool     overflow;
    uint32_t bytes;       /* every byte fed */
    uint32_t lines;       /* complete, non-empty lines */
    uint32_t overflows;   /* lines dropped for length */
} bt401_parser_t;

void bt401_parser_init(bt401_parser_t *p);
/* True when this byte completed a line; *out is its decoding (kind may be
 * BT401_MSG_OTHER for lines the driver has no use for). */
bool bt401_parser_feed(bt401_parser_t *p, uint8_t byte, bt401_msg_t *out);

/* Decodes one line (no line ending). Exposed for tests. */
void bt401_decode_line(const char *line, bt401_msg_t *out);

/* ---- link / call state ---------------------------------------------------- */

typedef enum {
    BT401_CALL_IDLE = 0,
    BT401_CALL_INCOMING,
    BT401_CALL_OUTGOING,
    BT401_CALL_ACTIVE,
} bt401_call_t;

typedef struct {
    bool         connected;       /* a phone is paired and connected (TS >= 01) */
    bool         music;           /* the phone is streaming music (TS+02) */
    int          ts;              /* last TS value, -1 until one arrives */
    bt401_call_t call;
    bool         dialing;         /* set by the driver just before it sends a dial command, so
                                   * TS+03 means "outgoing" instead of "incoming" */
    char         caller_id[BT401_NUMBER_MAX];
    char         name[BT401_NAME_MAX];     /* the module's own Bluetooth name */
    char         addr[18];                 /* its address, "AA:BB:CC:DD:EE:FF" */
    uint32_t     errors;          /* ER+n lines seen */
} bt401_state_t;

#define BT401_CHG_LINK    0x01   /* connected changed */
#define BT401_CHG_CALL    0x02   /* call state changed */
#define BT401_CHG_CALLER  0x04   /* caller id changed */
#define BT401_CHG_MUSIC   0x08
#define BT401_CHG_INFO    0x10   /* own name / address learned */

void bt401_state_init(bt401_state_t *s);
/* Folds one message into the state; returns BT401_CHG_* for what changed. */
unsigned bt401_state_apply(bt401_state_t *s, const bt401_msg_t *m);

/* One-line description of a message for logs. */
void bt401_describe(const bt401_msg_t *m, char *out, size_t out_max);

#ifdef __cplusplus
}
#endif
