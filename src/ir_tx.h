/*
 * Emetteur IR pour etiquettes ESL (PP4, porteuse ~1,25 MHz).
 * Port ESP32-S3 (peripherique RMT) du moteur IR de TagTinker.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef bool (*ir_stop_cb_t)(void);

/* Configure le RMT et la porteuse sur la broche IR. */
bool ir_init(void);
void ir_deinit(void);

/*
 * Envoie une trame deja construite (avec CRC).
 * repeats : nombre de repetitions en plus de l'envoi initial (comme sur Flipper).
 * gap_units : pause entre repetitions, en unites de 500 us.
 * should_stop : appele entre deux repetitions, peut etre NULL.
 * Retourne false si annule ou en cas d'erreur.
 */
bool ir_transmit(
    const uint8_t* data, size_t len, uint16_t repeats, uint8_t gap_units, ir_stop_cb_t should_stop);
