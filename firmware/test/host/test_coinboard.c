/* Host test of the coin board driver (coinboard.c): a simulated TCA9534A
 * (U1, 0x39) and ADS1115 (U4, 0x48) in place of the I2C bus, with a model of
 * the board's power stages behind the ADC. */
#include "coinboard.c"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
/* Not assert(): on the machine these were written on, a failed assert hangs
 * in abort() instead of exiting. */
#define assert(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

#define DEV_IO  ((void *)0x39)
#define DEV_ADC ((void *)0x48)

/* ---- U1 ---- */
static uint8_t io_reg[4];
static bool    io_dead, io_absent;
static uint8_t switches;            /* bit set = that coin switch closed */
static int     io_writes, io_reads;
static uint8_t first_write_reg = 0xFF;
static bool    hv_out_ever_high;    /* EN_HV, POL or FIRE driven high at any moment */
static uint8_t held_low;            /* pins a board fault holds low */

static void io_por(void)
{
    io_reg[REG_INPUT] = 0xFF; io_reg[REG_OUTPUT] = 0xFF; io_reg[REG_POLARITY] = 0; io_reg[REG_CONFIG] = 0xFF;
}
static uint8_t driven_high(void) { return (uint8_t)(io_reg[REG_OUTPUT] & (uint8_t)~io_reg[REG_CONFIG] & (uint8_t)~held_low); }
static bool pin(int bit) { return (driven_high() >> bit) & 1; }   /* pulldowns: an input pin reads low */

/* ---- the board behind U4 (1 ms steps, as in test_coin_fire.c) ---- */
static double rail = 4600, hv = 3900, i_ma;
static int    coil_ohm = 600, set_mv = 60000;
static void board_step(void)
{
    bool en = pin(COINBOARD_BIT_EN_HV), fire = pin(COINBOARD_BIT_FIRE);
    rail += ((en ? 20000 : 4600) - rail) * 0.05;
    if (en && rail > 8400 && hv < set_mv) hv += 150;
    if (hv < rail - 700) hv = rail - 700;
    double target = (fire && coil_ohm) ? hv / (coil_ohm + 10) : 0;
    i_ma += (target - i_ma) * 0.4;
    hv -= i_ma * 1e-3 / 220e-6;
}

/* ---- U4 ---- */
static bool     adc_absent, adc_dead;
static uint16_t adc_cfg;
static int      adc_conv_ch = -1;
static int      adc_starts[4];
static int16_t raw_for(double ain_mv)
{
    if (ain_mv > 3300) ain_mv = 3300;   /* VIO */
    return (int16_t)(ain_mv * 8);       /* 125 uV a count */
}
static int16_t adc_value(int ch)
{
    switch (ch) {
        case 0: return raw_for(rail * 10 / 66);
        case 1: return raw_for(hv * 27 / 1027);
        case 2: return raw_for(i_ma * 10);
        default: return raw_for(1234);
    }
}

/* ---- the I2C calls ---- */
static int add_device_calls;
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t b, const i2c_device_config_t *c, i2c_master_dev_handle_t *d)
{
    (void)b;
    add_device_calls++;
    assert(c->scl_speed_hz == 100000);
    assert(c->device_address == 0x39 || c->device_address == 0x48);
    *d = c->device_address == 0x39 ? DEV_IO : DEV_ADC;
    return ESP_OK;
}
esp_err_t i2c_master_probe(i2c_master_bus_handle_t b, uint16_t addr, int ms)
{
    (void)b; (void)ms;
    if (addr == 0x39) return (io_dead || io_absent) ? ESP_FAIL : ESP_OK;
    if (addr == 0x48) return (adc_dead || adc_absent) ? ESP_FAIL : ESP_OK;
    return ESP_FAIL;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t d, const uint8_t *w, size_t n, int ms)
{
    (void)ms;
    if (d == DEV_IO) {
        if (io_dead || io_absent) return ESP_FAIL;
        assert(n == 2 && w[0] != REG_INPUT && w[0] <= REG_CONFIG);
        if (first_write_reg == 0xFF) first_write_reg = w[0];
        io_reg[w[0]] = w[1];
        io_writes++;
        uint8_t hvbits = (1u << COINBOARD_BIT_EN_HV) | (1u << COINBOARD_BIT_POL) | (1u << COINBOARD_BIT_FIRE);
        if (driven_high() & hvbits) hv_out_ever_high = true;
        return ESP_OK;
    }
    if (adc_dead || adc_absent) return ESP_FAIL;
    assert(n == 3 && w[0] == ADS_REG_CFG);
    adc_cfg = (uint16_t)((w[1] << 8) | w[2]);
    assert(adc_cfg & 0x8000);                   /* start a conversion */
    assert(((adc_cfg >> 9) & 7) == 1);          /* +-4.096 V */
    assert((adc_cfg >> 8) & 1);                 /* single shot */
    assert(((adc_cfg >> 5) & 7) == 7);          /* 860 SPS */
    assert((adc_cfg & 3) == 3);                 /* comparator off */
    int mux = (adc_cfg >> 12) & 7;
    assert(mux >= 4);                           /* single-ended */
    adc_conv_ch = mux - 4;
    adc_starts[adc_conv_ch]++;
    return ESP_OK;
}
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t d, const uint8_t *w, size_t n, uint8_t *r, size_t m, int ms)
{
    (void)ms;
    if (d == DEV_IO) {
        if (io_dead || io_absent) return ESP_FAIL;
        assert(n == 1 && m == 1 && w[0] <= REG_CONFIG);
        io_reads++;
        /* Inputs read their pin: low while the switch is closed. Outputs read their driven level. */
        uint8_t in = (uint8_t)(~switches & 0x0F);
        io_reg[REG_INPUT] = (uint8_t)((in & io_reg[REG_CONFIG] & 0x0F) | driven_high());
        *r = io_reg[w[0]];
        return ESP_OK;
    }
    if (adc_dead || adc_absent) return ESP_FAIL;
    assert(n == 1 && m == 2 && w[0] == ADS_REG_CONV && adc_conv_ch >= 0);
    int16_t v = adc_value(adc_conv_ch);
    r[0] = (uint8_t)((uint16_t)v >> 8); r[1] = (uint8_t)v;
    return ESP_OK;
}
static int bus_resets;
esp_err_t i2c_master_bus_reset(i2c_master_bus_handle_t b) { (void)b; bus_resets++; return ESP_OK; }
esp_err_t board_i2c_init(void) { return ESP_OK; }
i2c_master_bus_handle_t board_i2c_bus(void) { return (void *)1; }
static int64_t now_us;
int64_t esp_timer_get_time(void) { return now_us; }
static shutdown_handler_t shutdown_fn;
esp_err_t esp_register_shutdown_handler(shutdown_handler_t h) { shutdown_fn = h; return ESP_OK; }

/* ---- NVS: one blob ---- */
static uint8_t nvs_blob[64];
static size_t  nvs_len;
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h) { (void)ns; (void)mode; *h = 1; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *v, size_t *len)
{
    (void)h; (void)k;
    if (!nvs_len) return ESP_ERR_NOT_FOUND;
    if (*len < nvs_len) return ESP_FAIL;
    memcpy(v, nvs_blob, nvs_len); *len = nvs_len;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t len)
{
    (void)h; (void)k; assert(len <= sizeof(nvs_blob));
    memcpy(nvs_blob, v, len); nvs_len = len;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }

/* ---- payphone and telephony ---- */
static payphone_relay_fn relay;
static int  board_line = -2;
static bool pp_enabled = true;
static int  coins_in[4], coin_cents_last, coin_line_last, coins_total;
static char last_log[160];
static int  logs;
void payphone_set_relay_handler(payphone_relay_fn fn) { relay = fn; }
void payphone_set_board_line(int line) { board_line = line; }
bool payphone_enabled(void) { return pp_enabled; }
void payphone_logf(int line, const char *fmt, ...)
{
    (void)line;
    va_list ap; va_start(ap, fmt); vsnprintf(last_log, sizeof(last_log), fmt, ap); va_end(ap);
    logs++;
}
esp_err_t telephony_coin_inject(int line, int cents, payphone_src_t src)
{
    assert(src == PAYPHONE_SRC_BOARD);
    coin_line_last = line; coin_cents_last = cents; coins_total++;
    if (cents == 25) coins_in[1]++; else if (cents == 10) coins_in[2]++; else if (cents == 5) coins_in[3]++;
    else coins_in[0]++;
    return ESP_OK;
}

/* ---- running it ---- */
static void run_ms(int ms)
{
    for (int k = 0; k < ms; k++) {
        now_us += 1000;
        board_step();
        if ((now_us / 1000) % COINBOARD_POLL_MS == 0) service_once(now_us);
    }
}
static void drop(int bit, int ms_closed)
{
    switches |= (uint8_t)(1u << bit); run_ms(ms_closed);
    switches &= (uint8_t)~(1u << bit); run_ms(60);
}
static void wait_idle(int max_ms)
{
    for (int k = 0; k < max_ms && (coin_fire_busy(&s_fire) || s_fire.pending >= 0); k++) run_ms(1);
}
static void set_cfg(bool en, int line, int coin4, bool autor)
{
    coinboard_cfg_t c = coinboard_cfg();
    c.enabled = en; c.line = (uint8_t)line; c.coin4_cents = (uint8_t)coin4; c.auto_relay = autor;
    coinboard_cfg_set(&c);
}

int main(void)
{
    /* Not enabled (the default): init and a while running never touch the bus. */
    io_por();
    coinboard_init();
    assert(relay && shutdown_fn && board_line == -1);
    run_ms(2000);
    assert(io_writes == 0 && io_reads == 0 && add_device_calls == 0);

    /* Enabled, with a chip left by a crash mid-pulse (EN_HV and FIRE high):
     * a restart's init turns them off first thing, output register first. */
    set_cfg(true, 1, 0, true);
    assert(board_line == 1);
    io_por();
    io_reg[REG_OUTPUT] = (1u << COINBOARD_BIT_EN_HV) | (1u << COINBOARD_BIT_FIRE);
    io_reg[REG_CONFIG] = COINBOARD_CONFIG;
    coinboard_init();               /* settings come back from NVS */
    assert(coinboard_cfg().enabled && coinboard_cfg().line == 1);
    assert(first_write_reg == REG_OUTPUT && io_reg[REG_OUTPUT] == 0 && io_reg[REG_CONFIG] == COINBOARD_CONFIG);

    /* From a powered-up chip, setup never drives an HV output high. */
    io_por(); hv_out_ever_high = false;
    atomic_store(&s_io_ok, false); s_last_retry_ms = 0;
    run_ms(10);
    assert(atomic_load(&s_io_ok) && !hv_out_ever_high);
    assert(io_reg[REG_CONFIG] == 0x0F && io_reg[REG_OUTPUT] == 0x00);   /* P7 an output, low */

    /* Coins: each switch is its coin, on the board's line. */
    run_ms(100);
    drop(COINBOARD_BIT_QUARTER, 30); assert(coins_in[1] == 1 && coin_cents_last == 25 && coin_line_last == 1);
    drop(COINBOARD_BIT_DIME, 30);    assert(coins_in[2] == 1 && coin_cents_last == 10);
    drop(COINBOARD_BIT_NICKEL, 30);  assert(coins_in[3] == 1 && coin_cents_last == 5);
    /* Coin 4 with no value: logged, not passed on. Then worth 50 c. */
    drop(COINBOARD_BIT_COIN4, 30);   assert(coins_in[0] == 0 && strstr(last_log, "coin 4"));
    set_cfg(true, 1, 50, true);
    drop(COINBOARD_BIT_COIN4, 30);   assert(coins_in[0] == 1 && coin_cents_last == 50);
    assert(coins_total == 4 && atomic_load(&s_coins) == 5);
    /* Twenty quarters in a row, 100 ms apart. */
    for (int i = 0; i < 20; i++) { drop(COINBOARD_BIT_QUARTER, 25); run_ms(15); }
    assert(coins_in[1] == 21);

    /* The ADC, idle: all four channels, about one every 100 ms. */
    memset(adc_starts, 0, sizeof(adc_starts));
    run_ms(2000);
    for (int ch = 0; ch < 4; ch++) assert(adc_starts[ch] >= 4 && adc_starts[ch] <= 6);
    coinboard_status_t st;
    coinboard_status(&st);
    assert(st.io_ok && st.adc_ok && st.enabled && st.active);
    assert(st.ain3_mv >= 1230 && st.ain3_mv <= 1235);
    assert(st.rail_mv > 4400 && st.rail_mv < 4800);     /* stage 1 idle */

    /* Collect, the way telephony asks for it: fires once, on the real chip. */
    int fires_seen = 0, fire_ms = 0, max_fire_ms = 0;
    bool pol_while_firing = false, pol_live_change = false, last_pol = false, last_fire = false;
    relay(1, PAYPHONE_COLLECT);
    for (int k = 0; k < 4000; k++) {
        run_ms(1);
        bool f = pin(COINBOARD_BIT_FIRE), p = pin(COINBOARD_BIT_POL);
        assert(!f || pin(COINBOARD_BIT_EN_HV));
        if (f && !last_fire) fires_seen++;
        fire_ms = f ? fire_ms + 1 : 0;
        if (fire_ms > max_fire_ms) max_fire_ms = fire_ms;
        if (f && p) pol_while_firing = true;
        if (p != last_pol && (f || last_fire || i_ma >= 20)) pol_live_change = true;
        last_fire = f; last_pol = p;
    }
    assert(fires_seen == 1 && !pol_while_firing && !pol_live_change);
    assert(max_fire_ms >= 99 && max_fire_ms <= 103);
    assert(s_fire.result == CF_RES_FIRED);
    assert(strstr(last_log, "collect: fired at"));
    printf("  logged: %s\n", last_log);
    /* While it charged, U4 read HV and the rail. */
    coinboard_status(&st);
    assert(st.state == CF_IDLE && st.fires == 1);

    /* Return: POL high for its pulse. A relay request for another line, or
     * with automatic collect/return off, does nothing. */
    relay(0, PAYPHONE_RETURN);
    run_ms(100);
    assert(!coin_fire_busy(&s_fire));
    set_cfg(true, 1, 50, false);
    relay(1, PAYPHONE_RETURN);
    run_ms(100);
    assert(!coin_fire_busy(&s_fire) && strstr(last_log, "off"));
    set_cfg(true, 1, 50, true);
    relay(1, PAYPHONE_RETURN);
    bool pol_fire = false;
    for (int k = 0; k < 4000; k++) { run_ms(1); if (pin(COINBOARD_BIT_FIRE) && pin(COINBOARD_BIT_POL)) pol_fire = true; }
    assert(pol_fire && s_fire.result == CF_RES_FIRED && !pin(COINBOARD_BIT_POL));

    /* Tests from the web UI. */
    assert(coinboard_test(CF_HV_TEST) == ESP_OK);
    run_ms(5000);
    assert(pin(COINBOARD_BIT_EN_HV) && !pin(COINBOARD_BIT_FIRE));
    coinboard_status(&st);
    assert(st.hv_mv > 55000 && st.hv_mv < 61000);
    printf("  HV test reads %d mV, rail %d mV\n", st.hv_mv, st.rail_mv);
    wait_idle(10000);
    assert(!pin(COINBOARD_BIT_EN_HV) && s_fire.result == CF_RES_HV_DONE);

    /* The chip loses power mid-charge: back to all inputs (the pulldowns hold
     * everything low), found from the outputs' readback within a few polls;
     * the sequence stops, never fires. */
    assert(coinboard_test(CF_COLLECT) == ESP_OK);
    run_ms(200);
    assert(s_fire.state == CF_CHARGE);
    io_por(); hv_out_ever_high = false;
    uint32_t fires_before = s_fire.fires;
    run_ms(4 * COINBOARD_POLL_MS);
    assert(atomic_load(&s_chip_resets) == 1 && io_reg[REG_CONFIG] == COINBOARD_CONFIG);
    assert(!pin(COINBOARD_BIT_EN_HV) && !pin(COINBOARD_BIT_FIRE) && s_fire.fires == fires_before);
    wait_idle(5000);
    assert(s_fire.result == CF_RES_FAULT && !hv_out_ever_high);

    /* FIRE held low by a fault on the board: seen as soon as FIRE goes out,
     * the sequence stops with a message naming it. */
    held_low = 1u << COINBOARD_BIT_FIRE;
    assert(coinboard_test(CF_COLLECT) == ESP_OK);
    wait_idle(8000);
    assert(s_fire.result == CF_RES_FAULT && strstr(s_fire.msg, "FIRE doesn't follow"));
    printf("  %s\n", s_fire.msg);
    held_low = 0;

    /* The cable comes off while charging: offline after about 100 ms, the bus
     * freed once, the line's coins stop; back when it's plugged in again,
     * with everything off. */
    assert(coinboard_test(CF_COLLECT) == ESP_OK);
    run_ms(200);
    io_dead = true;
    bus_resets = 0;
    run_ms(200);
    assert(!atomic_load(&s_io_ok) && bus_resets == 1);
    assert(coinboard_test(CF_RETURN) == ESP_ERR_INVALID_STATE);
    coinboard_status(&st);
    assert(!st.io_ok && st.closed == 0);
    io_dead = false; io_por();
    run_ms(CB_RETRY_MS + 10);
    assert(atomic_load(&s_io_ok) && io_reg[REG_OUTPUT] == 0);
    wait_idle(5000);
    run_ms(10);
    assert(!pin(COINBOARD_BIT_EN_HV) && !pin(COINBOARD_BIT_FIRE));

    /* No U4: the fixed wait, fired without a measurement. */
    adc_dead = true;
    run_ms(ADS_FAILS_TO_OFF * ADS_IDLE_MS + 200);   /* idle: one read every 100 ms */
    assert(!atomic_load(&s_adc_ok));
    assert(coinboard_test(CF_COLLECT) == ESP_OK);
    wait_idle(8000);
    assert(s_fire.result == CF_RES_UNMEASURED);
    coinboard_status(&st);
    assert(st.hv_mv == -1 && st.i_ma == -1);
    adc_dead = false;
    run_ms(ADS_RETRY_MS + 200);
    assert(atomic_load(&s_adc_ok));

    /* A restart while idle (OTA): the shutdown handler leaves the outputs low. */
    io_reg[REG_OUTPUT] = 0x10;
    shutdown_fn();
    assert(io_reg[REG_OUTPUT] == 0);

    /* Payphone features off: lets go of the board, outputs low, no more traffic. */
    pp_enabled = false;
    run_ms(50);
    assert(!atomic_load(&s_io_ok) && !coinboard_active());
    int w0 = io_writes, r0 = io_reads;
    run_ms(2000);
    assert(io_writes == w0 && io_reads == r0);
    assert(coinboard_test(CF_COLLECT) == ESP_ERR_INVALID_STATE);
    /* Back on: found again. */
    pp_enabled = true;
    run_ms(CB_RETRY_MS + CB_IDLE_WAIT_MS);
    assert(atomic_load(&s_io_ok));

    /* Switched off in the middle of a collect: it stops safely, then lets go. */
    assert(coinboard_test(CF_COLLECT) == ESP_OK);
    run_ms(300);
    set_cfg(false, 1, 50, true);
    assert(board_line == -1);
    run_ms(3000);
    assert(!atomic_load(&s_io_ok) && io_reg[REG_OUTPUT] == 0 && s_fire.state == CF_IDLE);

    /* Conversions. */
    assert(coinboard_hv_mv(raw_for(119000.0 * 27 / 1027)) / 1000 == 118);   /* rounding down */
    assert(coinboard_rail_mv(raw_for(3030)) / 100 == 199);
    assert(coinboard_isense_ma(raw_for(1000)) == 100);
    assert(coinboard_hv_mv(-5) == 0);

    printf("coin board driver: all checks passed\n");
    return 0;
}
