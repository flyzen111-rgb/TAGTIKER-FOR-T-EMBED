/*
 * LilyGo T-Embed CC1101 - brochage.
 * Source : depot officiel Xinyuan-LilyGO/T-Embed-CC1101 (section "Pins").
 */
#pragma once

#define BOARD_PWR_EN 15 /* alimentation des peripheriques : doit etre a HIGH */

/* Infrarouge */
#define BOARD_IR_TX 2 /* "BOARD_IR_EN" chez LilyGo : sortie vers la LED IR */
#define BOARD_IR_RX 1

/* Encodeur rotatif */
#define ENCODER_INA 4
#define ENCODER_INB 5
#define ENCODER_KEY 0

/* Bus SPI partage : ecran, carte SD, CC1101 */
#define BOARD_SPI_SCK  11
#define BOARD_SPI_MOSI 9
#define BOARD_SPI_MISO 10

#define BOARD_SD_CS   13
#define BOARD_LORA_CS 12 /* CC1101 : doit rester desactive (HIGH) */
#define DISPLAY_CS    41
#define DISPLAY_BL    21
