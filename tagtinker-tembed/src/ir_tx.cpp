/*
 * Emetteur IR - port du moteur de TagTinker (Flipper Zero) vers l'ESP32-S3.
 *
 * Sur Flipper : TIM1 genere une porteuse de 64 MHz / 51 = ~1,255 MHz et le
 * timing des symboles est fait en comptant les cycles CPU.
 * Ici : le peripherique RMT genere la porteuse (1,25 MHz) et cadence les
 * symboles en materiel, sans section critique.
 *
 * Encodage PP4 : 2 bits par symbole. Chaque symbole = un burst de porteuse
 * (~40,3 us) suivi d'un silence dont la duree code la valeur du symbole.
 * Durees d'origine (cycles a 64 MHz) : burst 2581, silences 3871 / 15483 /
 * 7741 / 11612 pour les symboles 0 / 1 / 2 / 3.
 * Le RMT tourne a 10 MHz (tick = 0,1 us) :
 *   burst   = 2581  / 64 MHz = 40,33 us  -> 403 ticks
 *   silence = 3871  / 64 MHz = 60,48 us  -> 605 ticks
 *             15483 / 64 MHz = 241,9 us  -> 2419 ticks
 *             7741  / 64 MHz = 120,95 us -> 1210 ticks
 *             11612 / 64 MHz = 181,44 us -> 1814 ticks
 */

#include "ir_tx.h"
#include "board_pins.h"

#include <Arduino.h>
#include <string.h>
#include "driver/rmt.h"

#define IR_RMT_CHANNEL RMT_CHANNEL_0
#define IR_RMT_CLK_DIV 8 /* 80 MHz / 8 = 10 MHz */
#define IR_CARRIER_HZ  1250000
#define IR_MAX_FRAME   255

static const uint16_t BURST_TICKS = 403;
static const uint16_t GAP_TICKS[4] = {605, 2419, 1210, 1814};
static const uint16_t TAIL_TICKS = 20;

static bool s_ready = false;
/* 4 symboles par octet + 1 burst final */
static rmt_item32_t s_items[IR_MAX_FRAME * 4 + 1];

bool ir_init(void) {
    if(s_ready) return true;

    rmt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.rmt_mode = RMT_MODE_TX;
    cfg.channel = IR_RMT_CHANNEL;
    cfg.gpio_num = (gpio_num_t)BOARD_IR_TX;
    cfg.clk_div = IR_RMT_CLK_DIV;
    cfg.mem_block_num = 4;
    cfg.tx_config.carrier_en = true;
    cfg.tx_config.carrier_freq_hz = IR_CARRIER_HZ;
    cfg.tx_config.carrier_duty_percent = 50;
    cfg.tx_config.carrier_level = RMT_CARRIER_LEVEL_HIGH;
    cfg.tx_config.idle_output_en = true;
    cfg.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;
    cfg.tx_config.loop_en = false;

    if(rmt_config(&cfg) != ESP_OK) return false;
    if(rmt_driver_install(cfg.channel, 0, 0) != ESP_OK) return false;

    s_ready = true;
    return true;
}

void ir_deinit(void) {
    if(!s_ready) return;
    rmt_driver_uninstall(IR_RMT_CHANNEL);
    s_ready = false;
    gpio_reset_pin((gpio_num_t)BOARD_IR_TX); /* detache la broche du RMT */
    pinMode(BOARD_IR_TX, OUTPUT);
    digitalWrite(BOARD_IR_TX, LOW);
}

static size_t build_items(const uint8_t* data, size_t len) {
    size_t n = 0;
    for(size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        for(int sym = 0; sym < 4; sym++) {
            uint8_t s = b & 0x03;
            b >>= 2;
            s_items[n].level0 = 1;
            s_items[n].duration0 = BURST_TICKS;
            s_items[n].level1 = 0;
            s_items[n].duration1 = GAP_TICKS[s];
            n++;
        }
    }
    /* burst de cloture, puis retour au repos */
    s_items[n].level0 = 1;
    s_items[n].duration0 = BURST_TICKS;
    s_items[n].level1 = 0;
    s_items[n].duration1 = TAIL_TICKS;
    n++;
    return n;
}

bool ir_transmit(
    const uint8_t* data, size_t len, uint16_t repeats, uint8_t gap_units, ir_stop_cb_t should_stop) {
    if(!s_ready) return false;
    if(!data || len == 0 || len > IR_MAX_FRAME) return false;

    size_t n = build_items(data, len);

    for(uint32_t rep = 0; rep <= repeats; rep++) {
        if(should_stop && should_stop()) return false;

        if(rmt_write_items(IR_RMT_CHANNEL, s_items, (int)n, true) != ESP_OK) return false;

        if(rep < repeats) {
            if(gap_units > 0) delayMicroseconds((uint32_t)gap_units * 500U);
            /* rend la main au systeme de temps en temps (watchdog) */
            if((rep % 5U) == 4U) delay(1);
        }
    }
    return true;
}
