/* bk3266_proto.h — the BK3266SR module's serial protocol, as pure C.
 *
 * No ESP-IDF dependencies on purpose: firmware/test/host/ compiles this on a
 * Mac and checks it against the example frames in the vendor's protocol
 * spreadsheet (Bluetooth/BK3266/reference_docs/), because the module itself
 * isn't needed to catch framing, checksum or state-mapping mistakes. The
 * UART/task side is bk3266.c.
 *
 * Everything here comes from the spreadsheet and the module manual, and is
 * UNVERIFIED against a real module until archive/hardware/bk3266.md says
 * otherwise -- in particular what the module sends when a call ends, and
 * whether 0x70/0x71/0x74 are really all "outgoing".
 *
 * Frame, both directions, 9600 8N1:
 *
 *   52 42 23  LEN  CMD  DATA...  CRC  23 42 52
 *
 *   LEN = 3 + number of DATA bytes (counts LEN, CMD and CRC themselves)
 *   CRC = (LEN + CMD + DATA...) & 0xFF
 *
 * Direction convention, read off the spreadsheet's worked examples:
 *   MCU -> module:  CMD = BK_MID_SEND (0x01), DATA[0] = a BK_CMD_* code
 *                   52 42 23 04 01 01 06 23 42 52   (BK_CMD_MATCH: pair)
 *   module -> MCU:  CMD = BK_MID_ACK (0x00), DATA[0] = a BK_ST_* status
 *                   52 42 23 04 00 61 65 23 42 52   (BK_ST_OK)
 *                   CMD = 0x03 + ASCII digits       (caller's number)
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BK_BAUD            9600
#define BK_FRAME_MAX_DATA  48
#define BK_FRAME_MAX_BYTES (3 + 1 + 1 + BK_FRAME_MAX_DATA + 1 + 3)

/* Outer CMD byte. */
enum {
    BK_MID_ACK        = 0x00, /* module -> MCU: DATA[0] is a BK_ST_* status */
    BK_MID_SEND       = 0x01, /* MCU -> module: DATA[0] is a BK_CMD_* code */
    BK_MID_PR         = 0x02, /* module -> MCU: the number of an outgoing call */
    BK_MID_IR         = 0x03, /* module -> MCU: the caller's number */
    BK_MID_VOL        = 0x04, /* module -> MCU: current volume */
    BK_MID_TWS        = 0x05, /* transmit-mode command (unused here) */
    BK_MID_TB         = 0x06, /* transmit-mode search result (unused here) */
    BK_MID_ADDR       = 0x07, /* module -> MCU: its own Bluetooth address */
    BK_MID_NAME       = 0x08, /* module -> MCU: its own Bluetooth name */
    BK_MID_PHONENAME  = 0x09, /* module -> MCU: the connected device's name */
    BK_MID_VERSION    = 0x0A, /* module -> MCU: software version */
};

/* MCU -> module command codes (DATA[0] of a BK_MID_SEND frame). */
enum {
    BK_CMD_MATCH        = 0x01, /* enter pairing */
    BK_CMD_CONNECTED    = 0x02, /* reconnect to the last device */
    BK_CMD_DISCONNECTED = 0x03, /* disconnect */
    BK_CMD_ACK          = 0x04, /* answer the call */
    BK_CMD_HFHUNG       = 0x05, /* reject the call */
    BK_CMD_NACK         = 0x06, /* hang up */
    BK_CMD_REDIAL       = 0x07, /* call back the last outgoing number */
    BK_CMD_VOLP         = 0x08,
    BK_CMD_VOLM         = 0x09,
    BK_CMD_CLEAR_MEMORY = 0x0A, /* clear the pairing list */
    BK_CMD_DEVICE_ADDR  = 0x11, /* query: own address */
    BK_CMD_DEVICE_NAME  = 0x12, /* query: own name */
    BK_CMD_PHONE_NAME   = 0x13, /* query: connected device's name */
    BK_CMD_VISON        = 0x14, /* query: software version */
    BK_CMD_ACL          = 0x15, /* query: is the ACL link up (-> C0/C1) */
    BK_CMD_MY           = 0x16, /* query: call status (-> M0..M4) */
    BK_CMD_VOL          = 0x1A, /* query: volume */
    BK_CMD_V0           = 0x18, /* open a 1 kHz test tone */
    BK_CMD_VS           = 0x19, /* stop the test tone */
    BK_CMD_EESET        = 0x1D, /* factory reset */
    BK_CMD_RECMODE      = 0x25, /* enter receive mode (the one we want) */
    BK_CMD_SENDMODE     = 0x24, /* enter transmit mode (restarts the module) */
    BK_CMD_BT_NAME      = 0x29, /* set the Bluetooth name */
};

/* module -> MCU status bytes (DATA[0] of a BK_MID_ACK frame). */
enum {
    BK_ST_OK    = 0x61,
    BK_ST_MA    = 0x62, /* time out */
    BK_ST_MB    = 0x63, /* play */
    BK_ST_ON    = 0x64, /* booted */
    BK_ST_II    = 0x65, /* link established */
    BK_ST_C0    = 0x66, /* ACL not connected (query reply) */
    BK_ST_C1    = 0x67, /* ACL connected (query reply) */
    BK_ST_M0    = 0x68, /* not connected (query reply) */
    BK_ST_M1    = 0x69, /* HFP connected, no call (query reply) */
    BK_ST_M2    = 0x70, /* calling out */
    BK_ST_M3    = 0x71, /* calling out */
    BK_ST_M4    = 0x72, /* in a call */
    BK_ST_IC    = 0x73, /* incoming call */
    BK_ST_ID    = 0x74, /* calling out */
    BK_ST_AUX   = 0x75, /* transmit mode */
    BK_ST_BT    = 0x76, /* Bluetooth (receive) mode */
    BK_ST_END   = 0x77,
    BK_ST_WC    = 0x78, /* transmit-mode pairing succeeded */
    BK_ST_WD    = 0x79, /* transmit-mode disconnect */
    BK_ST_IA    = 0x7A, /* receive-mode disconnect */
    BK_ST_IDLE  = 0x7B,
    BK_ST_MUTE  = 0x7C,
    BK_ST_UNMUTE = 0x7D,
};

typedef struct {
    uint8_t cmd;
    uint8_t n;                       /* DATA bytes */
    uint8_t data[BK_FRAME_MAX_DATA];
} bk_frame_t;

/* Builds a complete frame into out. Returns its length, or 0 if out is too
 * small or n is over BK_FRAME_MAX_DATA. */
size_t bk_build_frame(uint8_t cmd, const uint8_t *data, size_t n, uint8_t *out, size_t out_max);

/* A BK_MID_SEND frame carrying one BK_CMD_* code: what the MCU sends. */
size_t bk_build_command(uint8_t code, uint8_t *out, size_t out_max);

/* Byte-at-a-time frame parser. Resynchronises on the next header after any
 * bad byte, so line noise or a partial frame at power-up can't wedge it. */
typedef struct {
    uint8_t  state;
    uint8_t  len;
    uint8_t  idx;
    uint8_t  body[BK_FRAME_MAX_DATA + 2];  /* CMD + DATA + CRC */
    uint32_t bytes;      /* every byte fed */
    uint32_t frames;     /* good frames */
    uint32_t crc_errors; /* frames that ended correctly but failed the checksum */
    uint32_t resyncs;    /* bytes dropped while hunting for a header */
} bk_parser_t;

void bk_parser_init(bk_parser_t *p);
/* True when this byte completed a good frame (copied to *out). */
bool bk_parser_feed(bk_parser_t *p, uint8_t byte, bk_frame_t *out);

/* ---- what the module has told us ---------------------------------------- */

typedef enum {
    BK_CALL_IDLE = 0,
    BK_CALL_INCOMING,
    BK_CALL_OUTGOING,
    BK_CALL_ACTIVE,
} bk_call_t;

#define BK_NAME_MAX    32
#define BK_NUMBER_MAX  24
#define BK_ADDR_MAX    18
#define BK_VERSION_MAX 24

typedef struct {
    bool      booted;               /* saw BK_ST_ON this power-up */
    bool      connected;            /* a phone/laptop is linked */
    bk_call_t call;
    int       volume;               /* -1 until reported */
    int       last_status;          /* most recent BK_ST_*, -1 if none */
    char      caller_id[BK_NUMBER_MAX];
    char      phone_name[BK_NAME_MAX];
    char      local_name[BK_NAME_MAX];
    char      local_addr[BK_ADDR_MAX];
    char      version[BK_VERSION_MAX];
} bk_state_t;

#define BK_CHG_CALL   (1u << 0)  /* call state or caller ID changed */
#define BK_CHG_LINK   (1u << 1)  /* connected flag changed */
#define BK_CHG_INFO   (1u << 2)  /* a name/address/version/volume arrived */
#define BK_CHG_BOOT   (1u << 3)  /* the module (re)booted */

void bk_state_init(bk_state_t *s);

/* Applies one frame from the module to the state. Returns BK_CHG_* flags. */
unsigned bk_state_apply(bk_state_t *s, const bk_frame_t *f);

/* Human-readable one-liner for a frame ("status 0x73 IC incoming call"). */
void bk_describe(const bk_frame_t *f, char *out, size_t out_len);

const char *bk_status_name(uint8_t status);

#ifdef __cplusplus
}
#endif
