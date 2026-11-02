/* Host test of the v6 line 1 expander driver: a simulated TCA9534A in place of
 * the I2C bus. */
#include "line_expander.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---- simulated chip, in its power-on state ---- */
static uint8_t reg[4];
static int     pin_shk;
static bool    chip_dead;
static uint8_t log_reg[64], log_val[64];
static int     log_n;
static int     bus_resets;
static int     reads;
/* What the pins did: RM or F/R driven high at any moment. */
static bool    rm_ever_high, fr_ever_high;

static void chip_por(void)
{
    reg[REG_INPUT] = 0; reg[REG_OUTPUT] = 0xFF; reg[REG_POLARITY] = 0; reg[REG_CONFIG] = 0xFF;
}

static void watch_pins(void)
{
    uint8_t driven_high = (uint8_t)(reg[REG_OUTPUT] & (uint8_t)~reg[REG_CONFIG]);
    if (driven_high & (1u << BOARD_EXP_BIT_RM)) rm_ever_high = true;
    if (driven_high & (1u << BOARD_EXP_BIT_FR)) fr_ever_high = true;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t b, const i2c_device_config_t *c, i2c_master_dev_handle_t *d)
{
    assert(c->device_address == 0x38 && c->scl_speed_hz == 100000);
    *d = (void *)1;
    return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t d, const uint8_t *w, size_t n, int ms)
{
    if (chip_dead) return ESP_FAIL;
    assert(n == 2 && w[0] != REG_INPUT);
    reg[w[0]] = w[1];
    if (log_n < 64) { log_reg[log_n] = w[0]; log_val[log_n++] = w[1]; }
    watch_pins();
    return ESP_OK;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t d, const uint8_t *w, size_t n, uint8_t *r, size_t m, int ms)
{
    if (chip_dead) return ESP_FAIL;
    assert(n == 1 && m == 1);
    reads++;
    reg[REG_INPUT] = (uint8_t)(pin_shk << BOARD_EXP_BIT_SHK);
    *r = reg[w[0]];
    return ESP_OK;
}

esp_err_t i2c_master_bus_reset(i2c_master_bus_handle_t b) { bus_resets++; return ESP_OK; }
esp_err_t board_i2c_init(void) { return ESP_OK; }
i2c_master_bus_handle_t board_i2c_bus(void) { return (void *)1; }
int64_t esp_timer_get_time(void) { return 0; }

static int64_t now_us;
static void run_ms(int ms)
{
    for (int i = 0; i < ms; i++) { now_us += 1000; service_once(now_us); }
}

int main(void)
{
    /* Boots with a chip left ringing by a crash: RM and F/R outputs high. */
    chip_por();
    reg[REG_OUTPUT] = 0x03; reg[REG_CONFIG] = 0x0C;
    assert(line_exp_init() == ESP_OK);
    assert(log_reg[0] == REG_OUTPUT && log_val[0] == 0x00);
    assert(reg[REG_OUTPUT] == 0x00 && reg[REG_CONFIG] == 0x0C);

    /* From a power-on chip, nothing is ever driven high on the way up. */
    chip_por(); rm_ever_high = fr_ever_high = false; log_n = 0;
    assert(chip_setup() == ESP_OK);
    assert(!rm_ever_high && !fr_ever_high);
    assert(log_reg[0] == REG_OUTPUT);
    /* SHK and PD inputs, all else outputs (J10's P4-P7 mustn't float). */
    assert(reg[REG_CONFIG] == ((1u << BOARD_EXP_BIT_SHK) | (1u << BOARD_EXP_BIT_PD)));
    assert(atomic_load(&s_chip_ok));

    /* SHK follows the pin within a poll. */
    assert(line_exp_shk() == 0);
    pin_shk = 1; run_ms(EXP_POLL_MS + 1);
    assert(line_exp_shk() == 1);
    pin_shk = 0; run_ms(EXP_POLL_MS + 1);
    assert(line_exp_shk() == 0);

    /* Poll rate: about one read per EXP_POLL_MS, plus the odd verify. */
    reads = 0; run_ms(400);
    assert(reads >= 95 && reads <= 105);

    /* Outputs reach the chip on the next service, F/R and RM separately. */
    line_exp_set_rm(1); run_ms(1);
    assert(reg[REG_OUTPUT] == (1u << BOARD_EXP_BIT_RM));
    line_exp_set_fr(1); run_ms(1);
    assert(reg[REG_OUTPUT] == ((1u << BOARD_EXP_BIT_RM) | (1u << BOARD_EXP_BIT_FR)));
    line_exp_set_fr(0); line_exp_set_rm(0); run_ms(1);
    assert(reg[REG_OUTPUT] == 0);

    /* Powered down: PD driven low, and no SHK reads on the bus. */
    line_exp_set_pd_run(false); run_ms(1);
    assert(!(reg[REG_CONFIG] & (1u << BOARD_EXP_BIT_PD)) && !(reg[REG_OUTPUT] & (1u << BOARD_EXP_BIT_PD)));
    reads = 0; run_ms(100);
    assert(reads <= 1);
    line_exp_set_pd_run(true); run_ms(1);
    assert(reg[REG_CONFIG] & (1u << BOARD_EXP_BIT_PD));

    /* The chip loses power alone: back to all inputs, found and restored
     * without ever driving RM high, keeping a ring in progress. */
    line_exp_set_rm(1); run_ms(1);
    chip_por(); rm_ever_high = fr_ever_high = false;
    line_exp_set_rm(0);
    run_ms(EXP_VERIFY_MS + 1);
    assert(reg[REG_CONFIG] == CONFIG_RUN && reg[REG_OUTPUT] == 0);
    assert(!rm_ever_high && atomic_load(&s_chip_resets) == 1);

    /* Handset up, then the bus dies: a short blip keeps the reading... */
    pin_shk = 1; run_ms(EXP_POLL_MS + 1);
    assert(line_exp_shk() == 1);
    chip_dead = true;
    run_ms(EXP_POLL_MS * 5);
    assert(line_exp_shk() == 1 && atomic_load(&s_chip_ok));
    /* ...a lasting failure reads on-hook, and frees the bus once. */
    bus_resets = 0;
    run_ms(EXP_POLL_MS * (EXP_FAILS_TO_OFFLINE + 1));
    assert(line_exp_shk() == 0 && !atomic_load(&s_chip_ok) && bus_resets == 1);
    /* An output set while it's gone arrives with the recovery. */
    line_exp_set_fr(1);
    chip_dead = false; chip_por(); rm_ever_high = false;
    run_ms(EXP_RETRY_MS + EXP_POLL_MS + 2);
    assert(atomic_load(&s_chip_ok) && reg[REG_OUTPUT] == (1u << BOARD_EXP_BIT_FR));
    assert(line_exp_shk() == 1 && !rm_ever_high);

    printf("line expander: all checks passed\n");
    return 0;
}
